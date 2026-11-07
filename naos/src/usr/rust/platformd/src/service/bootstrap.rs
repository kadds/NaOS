use super::*;

pub(super) async fn run(context: Context) -> i64 {
    let resource =
        match servicekit::resolve_resource(context.service_directory_handle(), FIRMWARE_ROOT_URI) {
            Ok(resource) => resource,
            Err(error) => {
                log::error!("FirmwareRoot resolve failed: {error:?}");
                return 1;
            }
        };
    let endpoint = unsafe { ProtocolClientEndpoint::from_raw(resource.into_raw()) };
    let request = firmware_root::describe_request {};
    let mut request_wire = [0_u8; firmware_root::DESCRIBE_REQUEST_HEADER_BYTES];
    let mut invocation = match firmware_root::submit_describe(
        &endpoint,
        &request,
        ResourceTransferBuilder::new(),
        &mut request_wire,
        0,
    ) {
        Ok(invocation) => invocation,
        Err(error) => {
            log::error!("FirmwareRoot describe submit failed: {error:?}");
            return 1;
        }
    };
    if !completion_ready(invocation.get()).await {
        log::error!("FirmwareRoot describe did not complete");
        return 1;
    }
    // The unbounded IDL string contains the 32-byte kernel identity token in
    // addition to kind, revision, and PCI-fabric metadata.
    let mut response_wire = [0_u8; 64];
    match firmware_root::take_describe(&mut invocation, &mut response_wire) {
        Ok(response) => {
            log::info!(
                "ready kind={} firmware_version={} pci_fabric_generation={}",
                response.kind,
                response.firmware_version,
                response.pci_fabric_generation
            );
            // SAFETY: this syscall has no pointer arguments and only reports
            // the caller's kernel-assigned process ID.
            let process_id = unsafe { naos_sys::_s_current_pid() };
            if process_id <= 0 || response.firmware_version.is_empty() {
                log::error!("FirmwareRoot returned invalid platform identity");
                return 1;
            }
            let identity = rust_acpi_naos::FirmwareIdentity::new(
                response.firmware_version.to_owned(),
                process_id as u64,
            );
            let objects = match list_objects(&endpoint).await {
                Ok(objects) => objects,
                Err(error) => {
                    log::error!("FirmwareRoot list_objects failed: {error}");
                    return serve_firmware_unavailable(
                        &context,
                        identity,
                        response.pci_fabric_generation,
                        error,
                    )
                    .await;
                }
            };
            let relations = match list_relations(&endpoint).await {
                Ok(relations) => relations,
                Err(error) => {
                    log::error!("FirmwareRoot list_relations failed: {error}");
                    return serve_firmware_unavailable(
                        &context,
                        identity,
                        response.pci_fabric_generation,
                        error,
                    )
                    .await;
                }
            };
            if let Err(error) = catalog::validate_graph(&objects, &relations) {
                log::error!("firmware relation graph rejected: {error:?}");
                return serve_firmware_unavailable(
                    &context,
                    identity,
                    response.pci_fabric_generation,
                    format!("firmware relation graph rejected: {error:?}"),
                )
                .await;
            }
            log::info!(
                "firmware relation graph validated relations={}",
                relations.len()
            );
            for object in &objects {
                log::info!(
                    "firmware object id={} signature={:08x} aml_ordinal={}",
                    object.object_id,
                    u32::from_le_bytes(object.signature),
                    object.aml_load_ordinal
                );
            }
            if let Err(error) = catalog::validate_aml_order(&objects) {
                log::error!("firmware AML catalog rejected: {error:?}");
                return serve_firmware_unavailable(
                    &context,
                    identity,
                    response.pci_fabric_generation,
                    format!("firmware AML catalog rejected: {error:?}"),
                )
                .await;
            }
            let aml_object_count = objects
                .iter()
                .filter(|object| object.signature == *b"DSDT" || object.signature == *b"SSDT")
                .count();
            let mut aml_objects = Vec::<&catalog::Object>::new();
            if aml_objects.try_reserve_exact(aml_object_count).is_err() {
                log::error!("AML object list allocation failed");
                return serve_firmware_unavailable(
                    &context,
                    identity,
                    response.pci_fabric_generation,
                    "AML object list allocation failed".into(),
                )
                .await;
            }
            aml_objects.extend(
                objects
                    .iter()
                    .filter(|object| object.signature == *b"DSDT" || object.signature == *b"SSDT"),
            );
            // The catalog ordinal, not physical-address order, is the only
            // authority for AML load order.
            aml_objects.sort_by_key(|object| {
                if object.signature == *b"DSDT" {
                    0
                } else {
                    object.aml_load_ordinal
                }
            });
            let mut aml_tables = Vec::new();
            if aml_tables.try_reserve_exact(aml_objects.len()).is_err() {
                log::error!("AML table list allocation failed");
                return serve_firmware_unavailable(
                    &context,
                    identity,
                    response.pci_fabric_generation,
                    "AML table list allocation failed".into(),
                )
                .await;
            }
            let mut copied_aml_bytes = 0_u64;
            for object in aml_objects {
                let memory = match open_object(
                    &endpoint,
                    object.object_id,
                    &identity.firmware_version,
                )
                .await
                {
                    Ok(memory) => memory,
                    Err(error) => {
                        log::error!("AML object {} open failed: {error}", object.object_id);
                        return serve_firmware_unavailable(
                            &context,
                            identity,
                            response.pci_fabric_generation,
                            format!("AML object {} open failed: {error}", object.object_id),
                        )
                        .await;
                    }
                };
                let mapping = match memory.map_all(servicekit::sys::MEMORY_MAP_READ) {
                    Ok(mapping) => mapping,
                    Err(error) => {
                        log::error!(
                            "AML object {} read-only map failed: {error:?}",
                            object.object_id
                        );
                        return serve_firmware_unavailable(
                            &context,
                            identity,
                            response.pci_fabric_generation,
                            format!(
                                "AML object {} read-only map failed: {error:?}",
                                object.object_id
                            ),
                        )
                        .await;
                    }
                };
                let bytes = match catalog::copy_aml_table_bytes(
                    mapping.as_slice(),
                    object.length,
                    &mut copied_aml_bytes,
                ) {
                    Ok(bytes) => bytes,
                    Err(error) => {
                        log::error!(
                            "AML object {} snapshot length/budget/allocation rejected: {error:?}",
                            object.object_id
                        );
                        return serve_firmware_unavailable(
                            &context,
                            identity,
                            response.pci_fabric_generation,
                            format!(
                                "AML object {} snapshot rejected: {error:?}",
                                object.object_id
                            ),
                        )
                        .await;
                    }
                };
                aml_tables.push((object.signature, bytes));
            }
            let mut dsdt = None;
            let mut ssdts = Vec::new();
            if ssdts
                .try_reserve_exact(aml_tables.len().saturating_sub(1))
                .is_err()
            {
                log::error!("SSDT table list allocation failed");
                return serve_firmware_unavailable(
                    &context,
                    identity,
                    response.pci_fabric_generation,
                    "SSDT table list allocation failed".into(),
                )
                .await;
            }
            for (signature, bytes) in aml_tables {
                if signature == *b"DSDT" {
                    dsdt = Some(bytes);
                } else if signature == *b"SSDT" {
                    ssdts.push(bytes);
                }
            }
            let Some(dsdt) = dsdt else {
                log::error!("AML namespace has no DSDT snapshot");
                return serve_firmware_unavailable(
                    &context,
                    identity,
                    response.pci_fabric_generation,
                    "AML namespace has no DSDT snapshot".into(),
                )
                .await;
            };
            let profile = rust_acpi_naos::CompatibilityProfile::default();
            let backend = match region::KernelNativeRegionBackend::connect(
                context.service_directory_handle(),
                &identity.firmware_version,
            ) {
                Ok(backend) => backend,
                Err(error) => {
                    log::error!("AcpiResourceAuthority unavailable: {error}");
                    return serve_firmware_unavailable(
                        &context,
                        identity,
                        response.pci_fabric_generation,
                        format!("AcpiResourceAuthority unavailable: {error}"),
                    )
                    .await;
                }
            };
            let budget = rust_acpi_naos::ExecutionBudget::controlled_read_only();
            let hardware_side_effect_profile =
                rust_acpi_naos::HardwareSideEffectProfile::controlled_read_only();
            let mut executor = match ExecutorWorker::spawn(
                dsdt,
                ssdts,
                profile,
                budget,
                identity.clone(),
                hardware_side_effect_profile,
                backend.clone(),
            ) {
                Ok(executor) => executor,
                Err(error) => {
                    log::error!("{error}");
                    return serve_firmware_unavailable(
                        &context,
                        identity,
                        response.pci_fabric_generation,
                        error,
                    )
                    .await;
                }
            };
            macro_rules! publish_unavailable {
                ($reason : expr) => {{
                    return stop_executor_and_serve_unavailable(
                        &context,
                        executor,
                        identity,
                        response.pci_fabric_generation,
                        $reason.into(),
                    )
                    .await;
                }};
            }
            let s5_sleep_types = match executor.get_s5_sleep_types_async().await {
                Ok(types) => {
                    log::info!(
                        "ACPI S5 static sleep types available type_a={} type_b={}",
                        types.type_a,
                        types.type_b
                    );
                    Some(types)
                }
                Err(error) => {
                    if error == "GlobalLockOutcomeUnknown" || error == "NativeRegionOutcomeUnknown"
                    {
                        publish_unavailable!(format!(
                            "AML operation outcome is unknown while reading ACPI S5 state: {error}"
                        ));
                    }
                    log::warn!(
                        "ACPI S5 sleep transition unavailable; power-off requests will fail closed: {error}"
                    );
                    None
                }
            };
            let mut region_activation_failed = false;
            let mut discovery_failed = false;
            let aml_namespace_generation = match executor.summary() {
                Ok(summary) => summary.generation,
                Err(error) => {
                    log::error!("AML executor summary unavailable: {error}");
                    publish_unavailable!(format!("AML executor summary unavailable: {error}"));
                }
            };
            let compatibility_profile_hash = match executor.compatibility_profile_hash() {
                Ok(hash) => hash,
                Err(error) => {
                    log::error!("AML compatibility profile unavailable: {error}");
                    publish_unavailable!(format!("AML compatibility profile unavailable: {error}"));
                }
            };
            if compatibility_profile_hash == 0 {
                log::error!("AML compatibility profile produced an invalid zero hash");
                publish_unavailable!("AML compatibility profile produced an invalid zero hash");
            }
            let platform_topology_generation =
                match rust_acpi_naos::next_instance_generation(identity.process_id) {
                    Ok(generation) => generation,
                    Err(error) => {
                        log::error!("unable to allocate platform topology generation: {error:?}");
                        publish_unavailable!(format!(
                            "unable to allocate platform topology generation: {error:?}"
                        ));
                    }
                };
            if let Err(error) = backend
                .bind_platform_generation(aml_namespace_generation, platform_topology_generation)
            {
                log::error!("kernel rejected platform generation lineage: {error}");
                publish_unavailable!(format!(
                    "kernel rejected platform generation lineage: {error}"
                ));
            }
            let provisional_roots = Arc::new(match executor.get_pci_config_root_candidates() {
                Ok(roots) => roots,
                Err(error) => {
                    if error == "GlobalLockOutcomeUnknown" || error == "NativeRegionOutcomeUnknown"
                    {
                        publish_unavailable!(format!(
                            "AML operation outcome is unknown during PCI root hint discovery: {error}"
                        ));
                    }
                    discovery_failed = true;
                    log::warn!("static PCI_Config root hints unavailable: {error:?}");
                    Vec::new()
                }
            });
            let mut pci_root_scopes = BTreeMap::new();
            for root in provisional_roots.iter() {
                match backend.bind_pci_config_root(
                    aml_namespace_generation,
                    platform_topology_generation,
                    compatibility_profile_hash,
                    root,
                ) {
                    Ok(scope_id) => {
                        pci_root_scopes.insert(root.root_id, scope_id);
                    }
                    Err(error) => {
                        discovery_failed = true;
                        log::warn!(
                            "provisional PCI_Config root scope unavailable root_id={:016x}: {error}",
                            root.root_id
                        );
                    }
                }
            }
            let provisional_paths = match executor
                .pci_config_region_paths(Arc::clone(&provisional_roots))
            {
                Ok(paths) => paths,
                Err(error) => {
                    if error == "GlobalLockOutcomeUnknown" || error == "NativeRegionOutcomeUnknown"
                    {
                        publish_unavailable!(format!(
                            "AML operation outcome is unknown during provisional PCI path discovery: {error}"
                        ));
                    }
                    discovery_failed = true;
                    log::warn!("provisional PCI_Config AML paths unavailable: {error:?}");
                    Vec::new()
                }
            };
            match backend.install_pci_config_paths(provisional_paths, &pci_root_scopes) {
                Ok(count) => log::info!("provisional root PCI_Config selectors installed={count}"),
                Err(error) => {
                    discovery_failed = true;
                    log::warn!("provisional PCI_Config selector table degraded: {error}");
                }
            }
            let system_io_resources = match executor.preflight_system_io_resources() {
                Ok(resources) => resources,
                Err(error) => {
                    log::error!("SystemIO `_CRS` preflight failed closed: {error}");
                    publish_unavailable!(format!("SystemIO `_CRS` preflight failed: {error}"));
                }
            };
            match backend.install_system_io_resources(system_io_resources) {
                Ok(count) => log::info!("fixed consumer SystemIO `_CRS` ranges installed={count}"),
                Err(error) => {
                    log::error!("SystemIO `_CRS` allowlist installation failed closed: {error}");
                    publish_unavailable!(format!(
                        "SystemIO `_CRS` allowlist installation failed: {error}"
                    ));
                }
            }
            if let Err(error) = executor.activate_native_regions() {
                if error.contains("CleanupFailed") {
                    log::error!(
                        "ACPI region cleanup failed; terminating platformd for kernel owner cleanup: {error}"
                    );
                    if let Err(cleanup_error) = executor.shutdown() {
                        log::error!("ACPI region cleanup retry failed: {cleanup_error}");
                    }
                    return 1;
                }
                if let Err(cleanup_error) = executor.deactivate_native_regions() {
                    log::error!(
                        "ACPI region activation rollback could not be confirmed; terminating platformd for kernel owner cleanup: {cleanup_error}"
                    );
                    return 1;
                }
                region_activation_failed = true;
                discovery_failed = true;
                log::warn!("ACPI native OperationRegion _REG activation degraded: {error:?}");
            }
            let snapshot_policy = rust_acpi_naos::DeviceSnapshotPolicy {
                hardware: hardware_side_effect_profile,
                ..rust_acpi_naos::DeviceSnapshotPolicy::offline_default()
            };
            let devices = Arc::new(if region_activation_failed {
                Vec::new()
            } else {
                match executor.snapshot_devices(256, snapshot_policy) {
                    Ok(devices) => devices,
                    Err(error)
                        if error == "GlobalLockOutcomeUnknown"
                            || error == "NativeRegionOutcomeUnknown" =>
                    {
                        publish_unavailable!(
                            "AML hardware operation outcome is unknown; executor must be torn down"
                        );
                    }
                    Err(error) => {
                        log::warn!("offline ACPI device snapshot degraded: {error:?}");
                        discovery_failed = true;
                        Vec::new()
                    }
                }
            });
            let pci_discovery = if region_activation_failed || devices.is_empty() {
                rust_acpi_naos::PciRootDiscovery {
                    roots: Vec::new(),
                    device_candidates: 0,
                    hid_matches: 0,
                    osc_candidates: 0,
                    adr_zero_candidates: 0,
                    crs_address_spaces: 0,
                    bus_range_matches: 0,
                }
            } else {
                match executor.get_pci_roots() {
                    Ok(discovery) => discovery,
                    Err(error) => {
                        if error == "GlobalLockOutcomeUnknown"
                            || error == "NativeRegionOutcomeUnknown"
                        {
                            publish_unavailable!(format!(
                                "AML operation outcome is unknown during PCI root discovery: {error}"
                            ));
                        }
                        log::warn!("ACPI PCI root discovery degraded: {error:?}");
                        discovery_failed = true;
                        rust_acpi_naos::PciRootDiscovery {
                            roots: Vec::new(),
                            device_candidates: 0,
                            hid_matches: 0,
                            osc_candidates: 0,
                            adr_zero_candidates: 0,
                            crs_address_spaces: 0,
                            bus_range_matches: 0,
                        }
                    }
                }
            };
            let mut pci_root_descriptors = pci_discovery.roots;
            let filtered_roots = platform_topology::retain_functional_pci_roots(
                &mut pci_root_descriptors,
                devices.as_slice(),
            );
            if filtered_roots != 0 {
                log::warn!(
                    "ACPI PCI roots omitted because _STA did not confirm present and functioning count={filtered_roots}"
                );
            }
            let pci_roots = Arc::new(pci_root_descriptors);
            for root in pci_roots.iter() {
                match backend.bind_pci_config_root(
                    aml_namespace_generation,
                    platform_topology_generation,
                    compatibility_profile_hash,
                    root,
                ) {
                    Ok(scope_id) => {
                        pci_root_scopes.insert(root.root_id, scope_id);
                    }
                    Err(error) => {
                        discovery_failed = true;
                        log::warn!(
                            "PCI_Config root scope unavailable root_id={:016x}: {error}",
                            root.root_id
                        );
                    }
                }
            }
            match executor.pci_config_region_paths(Arc::clone(&pci_roots)) {
                Ok(paths) => match backend.install_pci_config_paths(paths, &pci_root_scopes) {
                    Ok(count) => log::info!("PCI_Config function selectors installed={count}"),
                    Err(error) => {
                        discovery_failed = true;
                        log::warn!("PCI_Config selector table degraded: {error}");
                    }
                },
                Err(error) => {
                    if error == "GlobalLockOutcomeUnknown" || error == "NativeRegionOutcomeUnknown"
                    {
                        publish_unavailable!(format!(
                            "AML operation outcome is unknown during PCI selector discovery: {error}"
                        ));
                    }
                    discovery_failed = true;
                    log::warn!("PCI_Config AML parent paths unavailable: {error:?}");
                }
            }
            let pci_device_paths = match executor
                .pci_device_function_paths(Arc::clone(&devices), Arc::clone(&pci_roots))
            {
                Ok(paths) => paths,
                Err(error) => {
                    if error == "GlobalLockOutcomeUnknown" || error == "NativeRegionOutcomeUnknown"
                    {
                        publish_unavailable!(format!(
                            "AML operation outcome is unknown during PCI function path discovery: {error}"
                        ));
                    }
                    discovery_failed = true;
                    log::warn!("PCI device _ADR paths unavailable: {error}");
                    Vec::new()
                }
            };
            if devices.len() > 256 {
                log::error!("ACPI device snapshot exceeded the platform topology bound");
                publish_unavailable!("ACPI device snapshot exceeded the platform topology bound");
            }
            let mut device_ids = BTreeMap::new();
            for device in devices.iter() {
                if device.namespace_generation != aml_namespace_generation
                    || device_ids.insert(device.device_id, ()).is_some()
                    || device.identifiers.len() > rust_acpi_naos::MAX_DEVICE_IDENTIFIERS
                    || device.resources.len() > 256
                {
                    log::error!(
                        "ACPI device snapshot violated topology identity or collection bounds"
                    );
                    publish_unavailable!(
                        "ACPI device snapshot violated topology identity or collection bounds"
                    );
                }
                log::info!(
                    "ACPI device path={} status={:x} initialized={} degraded={} resources={} routing_entries={}",
                    device.path,
                    device.status,
                    device.initialized,
                    device.degraded,
                    device.resources.len(),
                    device.routing_entries
                );
                for identifier in &device.identifiers {
                    log::info!(
                        "ACPI device identifier path={} kind={:?} value={:?}",
                        device.path,
                        identifier.kind,
                        identifier.value
                    );
                }
            }
            let mut pci_path_device_ids = BTreeMap::new();
            for path in &pci_device_paths {
                if path.device_id == 0
                    || path.root_id == 0
                    || path.device_function_path.is_empty()
                    || path.device_function_path.len()
                        > rust_acpi_naos::MAX_PCI_DEVICE_FUNCTION_PATH
                    || !device_ids.contains_key(&path.device_id)
                    || !pci_roots.iter().any(|root| root.root_id == path.root_id)
                    || pci_path_device_ids.insert(path.device_id, ()).is_some()
                {
                    log::error!("ACPI PCI device _ADR path violated identity or bounds");
                    publish_unavailable!("ACPI PCI device _ADR path violated identity or bounds");
                }
            }
            log::info!(
                "ACPI PCI root discovery candidates={} hid_matches={} osc_candidates={} adr_zero_candidates={} crs_address_spaces={} bus_range_matches={} roots={}",
                pci_discovery.device_candidates,
                pci_discovery.hid_matches,
                pci_discovery.osc_candidates,
                pci_discovery.adr_zero_candidates,
                pci_discovery.crs_address_spaces,
                pci_discovery.bus_range_matches,
                pci_roots.len()
            );
            for root in pci_roots.iter() {
                log::info!(
                    "ACPI PCI root discovered id={:016x} segment={} bus={}..{} aml_namespace_generation={:016x}",
                    root.root_id,
                    root.segment,
                    root.start_bus,
                    root.end_bus,
                    root.aml_namespace_generation
                );
            }
            // NaOS currently implements segment-group discovery, but no
            // native PCI control feature. Advertise only that OS support and
            // request no firmware ownership; the cached topology endpoint
            // rejects every other mask rather than re-entering AML on demand.
            let mut negotiated_features = Vec::new();
            let namespace = match executor.summary() {
                Ok(summary) => summary,
                Err(error) => {
                    log::error!("AML executor summary unavailable: {error}");
                    return 1;
                }
            };
            if namespace.global_lock_dependency {
                log::info!("AML namespace uses the host-managed ACPI Global Lock protocol");
            }
            if namespace.pic_degraded {
                log::warn!("offline AML _PIC evaluation degraded; no _PRT evaluation is admitted");
            }
            if namespace.deadline_exceeded {
                log::error!(
                    "AML method deadline exceeded; executor is sticky-degraded and rejects further AML work"
                );
            }
            let facs_present = objects.iter().any(|object| object.signature == *b"FACS");
            log::info!(
                "firmware catalog validated objects={} aml_namespace_generation={:016x} tables={} fuel_remaining={} facs_state_mapped={} native_region_writes=denied native_access_attempted={} native_access_authorized={} region_registration_degraded={} deadline_exceeded={}",
                objects.len(),
                namespace.generation,
                namespace.tables_loaded,
                namespace.opcode_fuel_remaining,
                false,
                namespace.native_access_attempted,
                namespace.native_access_authorized,
                namespace.region_registration_degraded,
                namespace.deadline_exceeded
            );
            if facs_present {
                log::info!("FACS is catalogued; mutable Global Lock state remains kernel-private");
            }

            let evidence_generation = platform_topology::PlatformGenerationSnapshot {
                firmware_version: identity.firmware_version.clone(),
                value: platform_pci_topology::PlatformGeneration {
                    aml_namespace_generation: namespace.generation,
                    platform_topology_generation,
                    pci_fabric_generation: response.pci_fabric_generation,
                    health: 0,
                    compatibility_profile_hash,
                },
            };
            let mut root_feature_validation_failed = false;
            for root in pci_roots.iter() {
                match executor.negotiate_pci_features(
                    &root.namespace_path,
                    platform_topology::IMPLEMENTED_PCI_ROOT_SUPPORT_MASK,
                    0,
                ) {
                    Ok(negotiation) => {
                        log::info!(
                            "ACPI PCI _OSC negotiated root_id={:016x} requested_support={:08x} requested_control={:08x} granted={:08x} denied={:08x} masked={:08x} query_errors={:08x} commit_errors={:08x} aml_namespace_generation={:016x}",
                            root.root_id,
                            negotiation.requested_support,
                            negotiation.requested_control,
                            negotiation.granted_control,
                            negotiation.denied_control,
                            negotiation.masked_control,
                            negotiation.query_errors,
                            negotiation.commit_errors,
                            negotiation.aml_namespace_generation
                        );
                        if platform_topology::pci_osc_negotiation_is_complete(&negotiation)
                            && negotiation.query_support == negotiation.requested_support
                        {
                            if let Some(scope_id) = pci_root_scopes.get(&root.root_id).copied() {
                                match platform_topology::build_pci_root_feature_evidence(
                                    root,
                                    scope_id,
                                    &evidence_generation,
                                    &negotiation,
                                ) {
                                    Ok(evidence) => {
                                        match executor.validate_pci_root_features(
                                            evidence,
                                            &identity.firmware_version,
                                        ) {
                                            Ok(()) => {
                                                log::info!(
                                                    "ACPI PCI _OSC evidence validated; no native control authority root_id={:016x}",
                                                    root.root_id
                                                )
                                            }
                                            Err(error) => {
                                                root_feature_validation_failed = true;
                                                log::warn!(
                                                    "kernel rejected complete _OSC root evidence root_id={:016x}: {error}",
                                                    root.root_id
                                                );
                                            }
                                        }
                                    }
                                    Err(error) => {
                                        root_feature_validation_failed = true;
                                        log::warn!(
                                            "_OSC root evidence failed kernel-validation preflight root_id={:016x}: {error:?}",
                                            root.root_id
                                        );
                                    }
                                }
                            }
                        }
                        negotiated_features.push((
                            root.root_id,
                            response.pci_fabric_generation,
                            negotiation,
                        ));
                    }
                    Err(error) => {
                        if error == "GlobalLockOutcomeUnknown"
                            || error == "NativeRegionOutcomeUnknown"
                        {
                            publish_unavailable!(format!(
                                "AML operation outcome is unknown during PCI _OSC negotiation: {error}"
                            ));
                        }
                        log::warn!(
                            "ACPI PCI _OSC negotiation degraded root_id={:016x}: {error:?}",
                            root.root_id
                        );
                    }
                }
            }

            let mut health = compute_platform_health(PlatformHealthInputs {
                pic_degraded: namespace.pic_degraded,
                region_registration_degraded: namespace.region_registration_degraded
                    || region_activation_failed,
                deadline_exceeded: namespace.deadline_exceeded,
                discovery_failed,
                negotiation_incomplete: negotiated_features.len() != pci_roots.len()
                    || root_feature_validation_failed
                    || negotiated_features.iter().any(|(_, _, negotiation)| {
                        !platform_topology::pci_osc_negotiation_is_complete(negotiation)
                    }),
            });
            let mut topology_generation = platform_topology::PlatformGenerationSnapshot {
                firmware_version: identity.firmware_version.clone(),
                value: platform_pci_topology::PlatformGeneration {
                    aml_namespace_generation: namespace.generation,
                    platform_topology_generation,
                    pci_fabric_generation: response.pci_fabric_generation,
                    health,
                    compatibility_profile_hash,
                },
            };
            if pci_roots.len() > rust_acpi_naos::MAX_PCI_ROOTS {
                log::error!("PCI root count exceeds the topology wire bound");
                publish_unavailable!("PCI root count exceeds the topology wire bound");
            }
            let mut topology_roots = Vec::with_capacity(pci_roots.len());
            let mut topology_resource_windows = Vec::new();
            for root in pci_roots.iter() {
                if root.resource_windows.is_empty()
                    || root.resource_windows.len() > rust_acpi_naos::MAX_PCI_ROOT_RESOURCE_WINDOWS
                {
                    log::error!(
                        "PCI root resource window count is invalid root_id={:016x} count={}",
                        root.root_id,
                        root.resource_windows.len()
                    );
                    publish_unavailable!(format!(
                        "PCI root resource window count is invalid root_id={:016x}",
                        root.root_id
                    ));
                }
                let Ok(resource_window_start) = u32::try_from(topology_resource_windows.len())
                else {
                    log::error!("PCI root resource window table offset overflowed");
                    publish_unavailable!("PCI root resource window table offset overflowed");
                };
                let Ok(resource_window_count) = u32::try_from(root.resource_windows.len()) else {
                    log::error!("PCI root resource window count overflowed");
                    publish_unavailable!("PCI root resource window count overflowed");
                };
                topology_resource_windows.extend(root.resource_windows.iter().map(|window| {
                    platform_pci_topology::PlatformPciResourceWindow {
                        resource_type: u32::from(window.resource_type),
                        descriptor_revision: window.descriptor_revision,
                        general_flags: window.general_flags,
                        type_specific_flags: window.type_specific_flags,
                        granularity: window.granularity,
                        minimum: window.minimum,
                        maximum: window.maximum,
                        translation_offset: window.translation_offset,
                        length: window.length,
                        type_specific_attributes: window.type_specific_attributes,
                    }
                }));
                topology_roots.push(platform_pci_topology::PlatformPciRoot {
                    root_id: root.root_id,
                    segment: u32::from(root.segment),
                    start_bus: u32::from(root.start_bus),
                    end_bus: u32::from(root.end_bus),
                    aml_namespace_generation: namespace.generation,
                    platform_topology_generation,
                    pci_fabric_generation: response.pci_fabric_generation,
                    device_id: root.device_id,
                    resource_window_start,
                    resource_window_count,
                    compatibility_profile_hash,
                });
            }
            if topology_resource_windows.len()
                > rust_acpi_naos::MAX_PCI_ROOTS * rust_acpi_naos::MAX_PCI_ROOT_RESOURCE_WINDOWS
            {
                log::error!("PCI root resource window table exceeds the topology wire bound");
                publish_unavailable!(
                    "PCI root resource window table exceeds the topology wire bound"
                );
            }
            let mut root_resource_attestations = BTreeMap::new();
            let mut device_resource_attestations = BTreeMap::new();
            let mut attestation_failed = false;
            for root in pci_roots.iter() {
                let Some(scope_id) = pci_root_scopes.get(&root.root_id).copied() else {
                    attestation_failed = true;
                    log::warn!(
                        "PCI root resource attestation skipped without kernel root scope root_id={:016x}",
                        root.root_id
                    );
                    continue;
                };
                match platform_topology::build_pci_root_resource_evidence(
                    root,
                    scope_id,
                    &topology_generation,
                ) {
                    Ok(evidence) => match executor.mint_pci_root_resource_attestation(evidence) {
                        Ok(attestation) => {
                            for path in pci_device_paths
                                .iter()
                                .filter(|path| path.root_id == root.root_id)
                            {
                                let Some(device) = devices
                                    .iter()
                                    .find(|device| device.device_id == path.device_id)
                                else {
                                    attestation_failed = true;
                                    log::warn!(
                                        "PCI device resource attestation skipped without device snapshot device_id={:016x}",
                                        path.device_id
                                    );
                                    continue;
                                };
                                if !device
                                    .address_spaces
                                    .iter()
                                    .any(|window| window.resource_type == 0)
                                {
                                    continue;
                                }
                                let evidence =
                                    match platform_topology::build_pci_device_resource_evidence(
                                        root,
                                        device,
                                        path,
                                        &topology_generation,
                                    ) {
                                        Ok(evidence) => evidence,
                                        Err(error) => {
                                            attestation_failed = true;
                                            log::warn!(
                                                "PCI device resource evidence rejected device_id={:016x}: {error:?}",
                                                device.device_id
                                            );
                                            continue;
                                        }
                                    };
                                match region::mint_pci_device_resource_attestation(
                                    &attestation,
                                    evidence,
                                ) {
                                    Ok(device_attestation) => {
                                        device_resource_attestations
                                            .insert(device.device_id, device_attestation);
                                    }
                                    Err(error) => {
                                        attestation_failed = true;
                                        log::warn!(
                                            "kernel rejected PCI device resource attestation device_id={:016x}: {error}",
                                            device.device_id
                                        );
                                    }
                                }
                            }
                            root_resource_attestations.insert(root.root_id, attestation);
                        }
                        Err(error) => {
                            attestation_failed = true;
                            log::warn!(
                                "kernel rejected PCI root resource attestation root_id={:016x}: {error}",
                                root.root_id
                            );
                        }
                    },
                    Err(error) => {
                        attestation_failed = true;
                        log::warn!(
                            "PCI root resource evidence invalid root_id={:016x}: {error:?}",
                            root.root_id
                        );
                    }
                }
            }
            if attestation_failed {
                health |= crate::topology_health::HEALTH_DEGRADED
                    | crate::topology_health::HEALTH_DISCOVERY_FAILED;
                topology_generation.health = health;
            }
            let degraded = health & crate::topology_health::HEALTH_DEGRADED != 0;
            let topology_features = negotiated_features
                .iter()
                .map(|(root_id, pci_fabric_generation, feature)| {
                    platform_pci_topology::PciRootFeatureSet {
                        root_id: *root_id,
                        requested_support: feature.requested_support,
                        requested_control: feature.requested_control,
                        query_support: feature.query_support,
                        query_granted_control: feature.query_granted_control,
                        commit_support: feature.commit_support,
                        granted_control: feature.granted_control,
                        denied_control: feature.denied_control,
                        masked_control: feature.masked_control,
                        query_errors: feature.query_errors,
                        commit_errors: feature.commit_errors,
                        aml_namespace_generation: feature.aml_namespace_generation,
                        platform_topology_generation,
                        compatibility_profile_hash: feature.compatibility_profile_hash,
                        pci_fabric_generation: *pci_fabric_generation,
                    }
                })
                .collect();
            let topology_state = Arc::new(tokio::sync::RwLock::new(TopologyState {
                generation: topology_generation,
                roots: topology_roots,
                resource_windows: topology_resource_windows,
                features: topology_features,
                devices,
                pci_device_paths,
            }));
            let (topology_invalidation, mut topology_invalidated) =
                platform_topology::TopologyInvalidation::channel();
            let root_resource_attestations =
                Arc::new(tokio::sync::Mutex::new(root_resource_attestations));
            let device_resource_attestations =
                Arc::new(tokio::sync::Mutex::new(device_resource_attestations));
            let mut topology_server = match Server::publish(
                &context.service_directory(),
                PLATFORM_PCI_TOPOLOGY_URI,
                &platform_pci_topology::protocol_descriptor(),
                platform_pci_topology::validate_request_transport_resources,
            )
            .await
            {
                Ok(server) => server,
                Err(error) => {
                    log::error!("PlatformPciTopology publish failed: {error:?}");
                    publish_unavailable!(format!("PlatformPciTopology publish failed: {error:?}"));
                }
            };
            let topology_state_for_service = topology_state.clone();
            tokio::spawn(async move {
                let mut handler = TopologyHandler {
                    state: topology_state_for_service,
                    firmware_version: String::new(),
                    root_resource_attestations,
                    device_resource_attestations,
                };
                match platform_topology::serve_until_invalidation(
                    topology_server.serve(&mut handler),
                    &mut topology_invalidated,
                )
                .await
                {
                    platform_topology::TopologyServeOutcome::ServerStopped(status) => {
                        log::error!("PlatformPciTopology server stopped status={status}");
                    }
                    platform_topology::TopologyServeOutcome::Invalidated(generation) => {
                        log::warn!(
                            "PlatformPciTopology generation invalidated={generation:016x}; closing clients to revoke stale local views"
                        );
                    }
                    platform_topology::TopologyServeOutcome::InvalidationChannelClosed => {
                        log::error!("PlatformPciTopology invalidation channel closed unexpectedly");
                    }
                }
            });
            log::info!(
                "PlatformPciTopology published generation={:016x} roots={} health={}",
                platform_topology_generation,
                pci_roots.len(),
                if degraded { "degraded" } else { "healthy" }
            );

            let sci_blocked_health = crate::topology_health::HEALTH_AML_DEADLINE_EXCEEDED
                | crate::topology_health::HEALTH_DISCOVERY_FAILED;
            let fixed_hardware = match describe_fixed_hardware(&context).await {
                Ok((endpoint, info)) => Some((Arc::new(endpoint), info)),
                Err(error) => {
                    log::warn!("AcpiFixedHardware inventory unavailable: {error}");
                    None
                }
            };
            if let Some((endpoint, info)) = fixed_hardware.as_ref() {
                match claim_transition_owner(endpoint, info).await {
                    Ok(()) => {
                        log::info!(
                            "AcpiFixedHardware transition owner claimed independently from SCI event execution"
                        );
                        if let Err(error) = publish_power_control(
                            &context,
                            Arc::clone(endpoint),
                            info.clone(),
                            s5_sleep_types,
                        )
                        .await
                        {
                            log::error!("PlatformPowerControl unavailable: {error}");
                        }
                    }
                    Err(error) => {
                        log::warn!("AcpiFixedHardware transition owner unavailable: {error}")
                    }
                }
            }
            let event_hardware = if health & sci_blocked_health != 0 {
                log::warn!(
                    "AcpiFixedHardware SCI claim skipped because AML/platform health forbids event execution health={health:#x}"
                );
                None
            } else if let Some((endpoint, info)) = fixed_hardware.as_ref() {
                match resolve_gpe_enable_masks(info, &executor).await {
                    Err(error)
                        if error == "GlobalLockOutcomeUnknown"
                            || error == "NativeRegionOutcomeUnknown" =>
                    {
                        log::error!(
                            "AML hardware-operation outcome is unknown during SCI GPE discovery; terminating platformd for kernel owner cleanup: {error}"
                        );
                        return 1;
                    }
                    Err(error) => {
                        log::warn!(
                            "SCI remains unclaimed because GPE handler discovery failed: {error}"
                        );
                        None
                    }
                    Ok(masks) => match claim_acpi_sci(endpoint, info).await {
                        Err(error) => {
                            log::warn!("AcpiFixedHardware SCI claim failed: {error}");
                            match mark_sci_consumer_failed(
                                endpoint,
                                &topology_state,
                                identity,
                                &topology_invalidation,
                            )
                            .await
                            {
                                Ok(generation) => {
                                    log::error!(
                                        "SCI claim failure reported to the kernel; degraded generation={generation:016x}; terminating platformd for kernel owner cleanup"
                                    );
                                    return 1;
                                }
                                Err(report_error) => {
                                    log::error!(
                                        "SCI claim failure remains unreported; terminating platformd for kernel owner cleanup: {report_error}"
                                    );
                                    return 1;
                                }
                            }
                        }
                        Ok(()) => match enable_gpe_handlers(endpoint, info, masks).await {
                            Ok(()) => {
                                log::info!(
                                    "AcpiFixedHardware SCI owner claimed; enabled AML GPE handlers={} gpe0_mask={:#018x} gpe1_mask={:#018x}",
                                    masks.handlers,
                                    masks.gpe0,
                                    masks.gpe1
                                );
                                Some((Arc::clone(endpoint), info.clone()))
                            }
                            Err(error) => {
                                log::error!("SCI GPE enable failed after claim: {error}");
                                match mark_sci_consumer_failed(
                                    endpoint,
                                    &topology_state,
                                    identity,
                                    &topology_invalidation,
                                )
                                .await
                                {
                                    Ok(generation) => log::error!(
                                        "SCI setup failure published degraded generation={generation:016x}"
                                    ),
                                    Err(report_error) => log::error!(
                                        "SCI setup failure could not be published: {report_error}"
                                    ),
                                }
                                return 1;
                            }
                        },
                    },
                }
            } else {
                None
            };
            if health & sci_blocked_health == 0 && event_hardware.is_none() {
                let updated_health =
                    crate::service::mark_initial_sci_failure(&topology_state).await;
                log::error!(
                    "ACPI SCI consumer unavailable; PlatformPciTopology health updated={updated_health:#x}"
                );
            }
            loop {
                let Some((endpoint, _info)) = event_hardware.as_ref() else {
                    tokio::time::sleep(core::time::Duration::from_secs(3600)).await;
                    continue;
                };
                let endpoint_handle = endpoint.get();
                let endpoint_handles = [endpoint_handle];
                let wait_outcome = tokio::select! {readiness = servicekit::wait_ready_async(&endpoint_handles, None) =>{
                        match readiness{Ok(event) if event.readable => SciWaitOutcome::Readable,
                                        Ok(event) =>{log::error!(
                                            "SCI readiness returned a non-readable event readable={} closed={} error={}",
                                            event.readable, event.read_closed || event.write_closed, event.error);
                    SciWaitOutcome::Failed
                }
                Err(error) =>
                {
                    log::error!("SCI readiness wait failed: {error:?}");
                    SciWaitOutcome::Failed
                }
                }
                }
                _ = tokio::time::sleep(core::time::Duration::from_secs(3600)) => { SciWaitOutcome::TimerElapsed }
                };

                match classify_sci_wait(wait_outcome) {
                    SciWaitAction::Drain => {
                        if let Some((endpoint, info)) = event_hardware.as_ref() {
                            if let Err(error) = drain_and_rearm_sci(endpoint, info, &executor).await
                            {
                                log::error!(
                                    "SCI drain/rearm failed; disabling the event consumer: {error}"
                                );
                                let generation = match mark_sci_consumer_failed(
                                    endpoint,
                                    &topology_state,
                                    identity,
                                    &topology_invalidation,
                                )
                                .await
                                {
                                    Ok(generation) => generation,
                                    Err(update_error) => {
                                        log::error!(
                                            "unable to publish SCI degradation: {update_error}"
                                        );
                                        return 1;
                                    }
                                };
                                log::error!(
                                    "SCI drain/rearm failed; published degraded generation={generation:016x}; no AML retry/rearm will occur; terminating platformd for kernel owner cleanup"
                                );
                                return 1;
                            }
                        }
                    }
                    SciWaitAction::Continue => {}
                    SciWaitAction::DisableAndExit => {
                        let Some((endpoint, _)) = event_hardware.as_ref() else {
                            return 1;
                        };
                        let generation = match mark_sci_consumer_failed(
                            endpoint,
                            &topology_state,
                            identity,
                            &topology_invalidation,
                        )
                        .await
                        {
                            Ok(generation) => generation,
                            Err(error) => {
                                log::error!("unable to publish SCI degradation: {error}");
                                return 1;
                            }
                        };
                        log::error!(
                            "SCI event consumer disabled after readiness failure; published degraded generation={generation:016x}; no AML retry/rearm will occur; terminating platformd for kernel owner cleanup"
                        );
                        return 1;
                    }
                }
            }
        }
        Err(error) => {
            log::error!("FirmwareRoot describe failed: {error:?}");
            1
        }
    }
}
