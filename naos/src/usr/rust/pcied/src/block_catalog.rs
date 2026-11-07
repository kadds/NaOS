use naos_idl::block_device_factory;
use naos_idl::pci_block_device_catalog;
use naos_idl::transport::{RpcMethodReply, RpcOutcome};
use naos_idl::{CallError, OwnedHandle, ResourceSlot};
use servicekit::server::{EndpointResource, Request, ServeHandler, Server, ServerError};
use servicekit::{Context, sys};
use std::time::Duration;
use std::vec::Vec;

pub(crate) const PCI_BLOCK_DEVICE_CATALOG_URI: &str = "naos://system/pci/block-device-catalog";
const DRIVER_BLOCK_DEVICE_FACTORY_URI: &str = "naos://service/pci/block-device-factory";
const MAX_CATALOG_ENTRIES: usize = 4096;

pub(crate) struct BlockFactoryProvider {
    pub identity: pci_block_device_catalog::PciBlockFactoryIdentity,
    pub directory: OwnedHandle,
}

pub(crate) struct BlockFactoryCatalog {
    firmware_version: String,
    pci_fabric_generation: u64,
    providers: Vec<BlockFactoryProvider>,
}

impl BlockFactoryCatalog {
    pub(crate) fn new(
        firmware_version: String,
        pci_fabric_generation: u64,
        mut providers: Vec<BlockFactoryProvider>,
    ) -> Result<Self, &'static str> {
        if firmware_version.is_empty() || pci_fabric_generation == 0 {
            return Err("PCI block catalog requires a firmware identity and fabric generation");
        }
        if providers.len() > MAX_CATALOG_ENTRIES {
            return Err("PCI block catalog exceeds its entry bound");
        }
        providers.sort_by_key(|provider| {
            let identity = provider.identity;
            (
                identity.segment,
                identity.bus,
                identity.device,
                identity.function,
            )
        });
        for (index, provider) in providers.iter().enumerate() {
            let identity = &provider.identity;
            if identity.factory_id == 0
                || identity.segment > u16::MAX as u32
                || identity.bus > u8::MAX as u32
                || identity.device > 31
                || identity.function > 7
            {
                return Err("PCI block catalog contains an invalid factory identity");
            }
            if providers[..index].iter().any(|previous| {
                previous.identity.factory_id == identity.factory_id
                    || (previous.identity.segment == identity.segment
                        && previous.identity.bus == identity.bus
                        && previous.identity.device == identity.device
                        && previous.identity.function == identity.function)
            }) {
                return Err("PCI block catalog contains a duplicate factory or BDF");
            }
        }
        Ok(Self {
            firmware_version,
            pci_fabric_generation,
            providers,
        })
    }

    pub(crate) fn entry_count(&self) -> usize {
        self.providers.len()
    }

    pub(crate) fn remove_factory(&mut self, factory_id: u64) -> bool {
        let previous_len = self.providers.len();
        self.providers
            .retain(|provider| provider.identity.factory_id != factory_id);
        self.providers.len() != previous_len
    }

    pub(crate) async fn publish(context: Context) -> Result<Server, ServerError> {
        let directory = context.service_directory();
        let descriptor = pci_block_device_catalog::protocol_descriptor();
        Server::publish(
            &directory,
            PCI_BLOCK_DEVICE_CATALOG_URI,
            &descriptor,
            pci_block_device_catalog::validate_request_transport_resources,
        )
        .await
    }
}

impl ServeHandler for BlockFactoryCatalog {
    async fn handle(&mut self, server: &mut Server, request: Request) {
        let dispatched = {
            let rpc_request = request.rpc_request();
            pci_block_device_catalog::dispatch_servicekit(&rpc_request, &mut (), self).await
        };
        if let Err(error) = server.respond_dispatch(request, dispatched).await {
            log::warn!("PCI block catalog response failed: {error:?}");
        }
    }
}

impl pci_block_device_catalog::PciBlockDeviceCatalogServiceKitHandler<()> for BlockFactoryCatalog {
    type Resource = EndpointResource;

    async fn list<'s>(
        &'s mut self,
        _context: &mut (),
        value: pci_block_device_catalog::list_request<'_>,
    ) -> Result<RpcMethodReply<pci_block_device_catalog::list_response, Self::Resource>, RpcOutcome>
    {
        let starts_snapshot = value.firmware_version.is_empty() && value.pci_fabric_generation == 0;
        if value.firmware_version.is_empty() != (value.pci_fabric_generation == 0)
            || (!starts_snapshot
                && (value.firmware_version != self.firmware_version
                    || value.pci_fabric_generation != self.pci_fabric_generation))
        {
            return Err(service_outcome(pci_block_device_catalog::LIST_ERROR_ESTALE));
        }
        if starts_snapshot && value.cursor != 0 {
            return Err(service_outcome(pci_block_device_catalog::LIST_ERROR_EINVAL));
        }
        let start = usize::try_from(value.cursor)
            .map_err(|_| service_outcome(pci_block_device_catalog::LIST_ERROR_EINVAL))?;
        if start > self.providers.len() {
            return Err(service_outcome(pci_block_device_catalog::LIST_ERROR_EINVAL));
        }
        let end = start.saturating_add(16).min(self.providers.len());
        let mut factories = [pci_block_device_catalog::PciBlockFactoryIdentity::default(); 16];
        for (destination, provider) in factories.iter_mut().zip(&self.providers[start..end]) {
            *destination = provider.identity;
        }
        let next_cursor = if end < self.providers.len() {
            end as u64
        } else {
            0
        };
        let response = pci_block_device_catalog::list_response {
            value: pci_block_device_catalog::PciBlockFactoryPage {
                pci_fabric_generation: self.pci_fabric_generation,
                next_cursor,
                count: (end - start) as u32,
                factories,
            },
            firmware_version: self.firmware_version.as_str(),
        };
        Ok(RpcMethodReply::new(response))
    }

    async fn open_factory<'s>(
        &'s mut self,
        _context: &mut (),
        value: pci_block_device_catalog::open_factory_request<'_>,
    ) -> Result<
        RpcMethodReply<pci_block_device_catalog::open_factory_response, Self::Resource>,
        RpcOutcome,
    > {
        if value.factory_id == 0 {
            return Err(service_outcome(
                pci_block_device_catalog::OPEN_FACTORY_ERROR_EINVAL,
            ));
        }
        if value.firmware_version != self.firmware_version
            || value.pci_fabric_generation != self.pci_fabric_generation
        {
            return Err(service_outcome(
                pci_block_device_catalog::OPEN_FACTORY_ERROR_ESTALE,
            ));
        }
        let provider = self
            .providers
            .iter()
            .find(|provider| provider.identity.factory_id == value.factory_id)
            .ok_or_else(|| service_outcome(pci_block_device_catalog::OPEN_FACTORY_ERROR_ENODEV))?;
        let endpoint = servicekit::connect_until(
            provider.directory.get(),
            DRIVER_BLOCK_DEVICE_FACTORY_URI,
            block_device_factory::PROTOCOL_UUID,
            block_device_factory::PROTOCOL_RIGHTS,
            block_device_factory::PROTOCOL_REVISION,
            block_device_factory::PROTOCOL_FEATURES,
            Duration::from_secs(2),
        )
        .await
        .map_err(|error| service_outcome(map_connect_error(error)))?;
        let handle = unsafe { OwnedHandle::from_raw(endpoint.into_raw()) };
        let info = naos_idl::handle_info(handle.get())
            .map_err(|_| service_outcome(pci_block_device_catalog::OPEN_FACTORY_ERROR_EIO))?;
        if info.binding != sys::BINDING_CLIENT_END
            || info.protocol_uuid.bytes != block_device_factory::PROTOCOL_UUID
            || info.scope != block_device_factory::PROTOCOL_SCOPE
            || info.revision != block_device_factory::PROTOCOL_REVISION
            || info.features != block_device_factory::PROTOCOL_FEATURES
            || info.meta_rights & (sys::RIGHT_TRANSFER | sys::RIGHT_WAIT)
                != (sys::RIGHT_TRANSFER | sys::RIGHT_WAIT)
            || info.protocol_rights & block_device_factory::PROTOCOL_RIGHTS
                != block_device_factory::PROTOCOL_RIGHTS
        {
            return Err(service_outcome(
                pci_block_device_catalog::OPEN_FACTORY_ERROR_EACCES,
            ));
        }
        let meta_rights = sys::RIGHT_TRANSFER | sys::RIGHT_WAIT | sys::RIGHT_INSPECT;
        let protocol_rights = block_device_factory::PROTOCOL_RIGHTS;
        let factory = EndpointResource::from_owned_capability(
            handle,
            &block_device_factory::protocol_descriptor(),
            meta_rights,
            protocol_rights,
        )
        .map_err(|_| service_outcome(pci_block_device_catalog::OPEN_FACTORY_ERROR_EACCES))?;
        let response_value = pci_block_device_catalog::open_factory_response {
            factory: ResourceSlot::new(0)
                .ok_or_else(|| service_outcome(pci_block_device_catalog::OPEN_FACTORY_ERROR_EIO))?,
        };
        let mut reply = RpcMethodReply::new(response_value);
        reply.resources.push(factory);
        Ok(reply)
    }
}

fn service_outcome(protocol_error: i64) -> RpcOutcome {
    RpcOutcome {
        execution: 0,
        reason: 0,
        protocol_error,
    }
}

fn map_connect_error(error: CallError) -> i64 {
    match error {
        CallError::Outcome { protocol_error, .. }
            if protocol_error == -2 || protocol_error == 2 =>
        {
            pci_block_device_catalog::OPEN_FACTORY_ERROR_ENODEV
        }
        CallError::Status(sys::STATUS_ACCESS_DENIED) => {
            pci_block_device_catalog::OPEN_FACTORY_ERROR_EACCES
        }
        CallError::Status(sys::STATUS_WAIT_TIMED_OUT | sys::STATUS_PEER_CLOSED) => {
            pci_block_device_catalog::OPEN_FACTORY_ERROR_ENODEV
        }
        CallError::Status(_) => pci_block_device_catalog::OPEN_FACTORY_ERROR_EIO,
        _ => pci_block_device_catalog::OPEN_FACTORY_ERROR_EIO,
    }
}
