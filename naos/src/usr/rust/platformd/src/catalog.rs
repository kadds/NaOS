//! FirmwareRoot catalog checks shared by platformd and host-side corpus tests.
//!
//! This is deliberately not an AML interpreter. It validates the immutable
//! object graph supplied by the kernel and enforces DSDT-first/SSDT-ordinal
//! ordering before a future rust-acpi-naos backend is allowed to execute it.

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Object {
    pub object_id: u64,
    pub signature: [u8; 4],
    pub length: u64,
    pub root_ordinal: u64,
    pub aml_load_ordinal: u64,
    pub firmware_version: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Relation {
    pub from_object: u64,
    pub to_object: u64,
    pub kind: u32,
    pub ordinal: u32,
    pub firmware_version: String,
}

pub const RELATION_ROOT_CHILD: u32 = 0;
pub const RELATION_FADT_DSDT: u32 = 1;
pub const RELATION_FADT_FACS: u32 = 2;
/// FirmwareRoot transports ordinals as u64, but the kernel uses the source
/// table's u32::MAX sentinel for objects that have no root-table ordinal.
pub const NO_ROOT_ORDINAL: u64 = u32::MAX as u64;
pub(crate) const MAX_FIRMWARE_TABLE_COUNT: usize = rust_acpi_naos::MAX_AML_TABLE_COUNT;
pub(crate) const MAX_TABLE_BYTES: u64 = rust_acpi_naos::MAX_AML_TABLE_BYTES;
pub(crate) const MAX_CATALOG_BYTES: u64 = rust_acpi_naos::MAX_AML_CATALOG_BYTES;
/// At most one relation per root-table child plus the DSDT and FACS edges
/// owned by the single validated FADT.
pub(crate) const MAX_FIRMWARE_RELATION_COUNT: usize = MAX_FIRMWARE_TABLE_COUNT + 1;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CatalogError {
    TooManyRecords,
    AllocationFailed,
    InvalidTableLength,
    TableTooLarge,
    CatalogTooLarge,
    MissingDsdt,
    DuplicateObject,
    InvalidAmlOrder,
    UnknownRelationObject,
    InvalidDsdtRelation,
    InconsistentIdentity,
    InvalidRootRelation,
}

/// Reserve capacity for one untrusted catalog page before appending any of
/// its records. This keeps page aggregation within the kernel catalog bound
/// and turns allocator failure into a recoverable validation error.
pub(crate) fn reserve_append<T>(
    values: &mut Vec<T>,
    additional: usize,
    limit: usize,
) -> Result<(), CatalogError> {
    let new_len = values
        .len()
        .checked_add(additional)
        .ok_or(CatalogError::TooManyRecords)?;
    if new_len > limit {
        return Err(CatalogError::TooManyRecords);
    }
    values
        .try_reserve_exact(additional)
        .map_err(|_| CatalogError::AllocationFailed)
}

/// Make a bounded copy of one AML table while accounting its bytes against
/// the whole firmware catalog. The aggregate counter changes only after the
/// fallible allocation and copy have succeeded.
pub(crate) fn copy_aml_table_bytes(
    bytes: &[u8],
    declared_length: u64,
    copied_bytes: &mut u64,
) -> Result<Vec<u8>, CatalogError> {
    const SDT_HEADER_BYTES: u64 = 36;
    if declared_length > MAX_TABLE_BYTES {
        return Err(CatalogError::TableTooLarge);
    }
    if declared_length < SDT_HEADER_BYTES
        || usize::try_from(declared_length).ok() != Some(bytes.len())
    {
        return Err(CatalogError::InvalidTableLength);
    }
    let next_total = copied_bytes
        .checked_add(declared_length)
        .filter(|total| *total <= MAX_CATALOG_BYTES)
        .ok_or(CatalogError::CatalogTooLarge)?;

    let mut copy = Vec::new();
    copy.try_reserve_exact(bytes.len())
        .map_err(|_| CatalogError::AllocationFailed)?;
    copy.extend_from_slice(bytes);
    *copied_bytes = next_total;
    Ok(copy)
}

pub fn validate_aml_order(objects: &[Object]) -> Result<(), CatalogError> {
    if objects.len() > MAX_FIRMWARE_TABLE_COUNT {
        return Err(CatalogError::TooManyRecords);
    }
    let identity = objects
        .first()
        .map(|object| object.firmware_version.as_str());
    if identity.is_some_and(str::is_empty) {
        return Err(CatalogError::InconsistentIdentity);
    }
    if objects
        .iter()
        .any(|object| Some(object.firmware_version.as_str()) != identity)
    {
        return Err(CatalogError::InconsistentIdentity);
    }
    let mut dsdt = None;
    let mut ssdt_ordinals = [false; MAX_FIRMWARE_TABLE_COUNT];
    let mut aml_catalog_bytes = 0_u64;
    for object in objects {
        if object.signature == *b"DSDT" {
            validate_aml_table_descriptor(object.length, &mut aml_catalog_bytes)?;
            if dsdt.replace(object.object_id).is_some() {
                return Err(CatalogError::DuplicateObject);
            }
            if object.aml_load_ordinal != 0 {
                return Err(CatalogError::InvalidAmlOrder);
            }
        } else if object.signature == *b"SSDT" {
            validate_aml_table_descriptor(object.length, &mut aml_catalog_bytes)?;
            let ordinal = usize::try_from(object.aml_load_ordinal)
                .map_err(|_| CatalogError::InvalidAmlOrder)?;
            if ordinal == 0 || ordinal > ssdt_ordinals.len() || ssdt_ordinals[ordinal - 1] {
                return Err(CatalogError::InvalidAmlOrder);
            }
            ssdt_ordinals[ordinal - 1] = true;
        }
    }
    if dsdt.is_none() {
        return Err(CatalogError::MissingDsdt);
    }
    let mut saw_hole = false;
    for present in ssdt_ordinals {
        if !present {
            saw_hole = true;
        } else if saw_hole {
            return Err(CatalogError::InvalidAmlOrder);
        }
    }
    Ok(())
}

fn validate_aml_table_descriptor(length: u64, total_bytes: &mut u64) -> Result<(), CatalogError> {
    const SDT_HEADER_BYTES: u64 = 36;
    if length < SDT_HEADER_BYTES {
        return Err(CatalogError::InvalidTableLength);
    }
    if length > MAX_TABLE_BYTES {
        return Err(CatalogError::TableTooLarge);
    }
    *total_bytes = total_bytes
        .checked_add(length)
        .filter(|total| *total <= MAX_CATALOG_BYTES)
        .ok_or(CatalogError::CatalogTooLarge)?;
    Ok(())
}

/// Validate the opaque FirmwareRoot graph before any table bytes are opened.
/// The graph, not a physical address or caller-selected object ordering, is
/// the authority for the FADT indirections used by the ACPI backend.
pub fn validate_graph(objects: &[Object], relations: &[Relation]) -> Result<(), CatalogError> {
    if objects.len() > MAX_FIRMWARE_TABLE_COUNT || relations.len() > MAX_FIRMWARE_RELATION_COUNT {
        return Err(CatalogError::TooManyRecords);
    }
    let contains = |id| objects.iter().any(|object| object.object_id == id);
    let identity = objects
        .first()
        .map(|object| object.firmware_version.as_str());
    let mut root_id = None;
    for object in objects
        .iter()
        .filter(|object| object.signature == *b"RSDT" || object.signature == *b"XSDT")
    {
        if root_id.replace(object.object_id).is_some() {
            return Err(CatalogError::InvalidRootRelation);
        }
    }
    let root_id = root_id.ok_or(CatalogError::InvalidRootRelation)?;
    let mut fadt_dsdt = None;
    let mut fadt_facs = None;
    let mut root_child_ordinals = [false; 1024];
    let mut described_root_child_ordinals = [false; 1024];
    for object in objects {
        if object.object_id == root_id {
            if object.root_ordinal != NO_ROOT_ORDINAL {
                return Err(CatalogError::InvalidRootRelation);
            }
            continue;
        }
        if object.root_ordinal == NO_ROOT_ORDINAL {
            continue;
        }
        let ordinal =
            usize::try_from(object.root_ordinal).map_err(|_| CatalogError::InvalidRootRelation)?;
        if ordinal >= described_root_child_ordinals.len() || described_root_child_ordinals[ordinal]
        {
            return Err(CatalogError::InvalidRootRelation);
        }
        described_root_child_ordinals[ordinal] = true;
    }
    for relation in relations {
        if Some(relation.firmware_version.as_str()) != identity {
            return Err(CatalogError::InconsistentIdentity);
        }
        if !contains(relation.from_object) || !contains(relation.to_object) {
            return Err(CatalogError::UnknownRelationObject);
        }
        match relation.kind {
            RELATION_ROOT_CHILD => {
                if relation.from_object != root_id {
                    return Err(CatalogError::InvalidRootRelation);
                }
                let child = objects
                    .iter()
                    .find(|object| object.object_id == relation.to_object)
                    .ok_or(CatalogError::UnknownRelationObject)?;
                let ordinal = usize::try_from(relation.ordinal)
                    .map_err(|_| CatalogError::InvalidRootRelation)?;
                if child.root_ordinal != relation.ordinal as u64
                    || ordinal >= root_child_ordinals.len()
                    || root_child_ordinals[ordinal]
                {
                    return Err(CatalogError::InvalidRootRelation);
                }
                root_child_ordinals[ordinal] = true;
            }
            RELATION_FADT_DSDT => {
                let source = objects
                    .iter()
                    .find(|object| object.object_id == relation.from_object)
                    .ok_or(CatalogError::UnknownRelationObject)?;
                let target = objects
                    .iter()
                    .find(|object| object.object_id == relation.to_object)
                    .ok_or(CatalogError::UnknownRelationObject)?;
                if source.signature != *b"FACP"
                    || target.signature != *b"DSDT"
                    || relation.ordinal != 0
                    || fadt_dsdt.is_some()
                {
                    return Err(CatalogError::InvalidDsdtRelation);
                }
                fadt_dsdt = Some(relation.to_object);
            }
            RELATION_FADT_FACS => {
                let source = objects
                    .iter()
                    .find(|object| object.object_id == relation.from_object)
                    .ok_or(CatalogError::UnknownRelationObject)?;
                let target = objects
                    .iter()
                    .find(|object| object.object_id == relation.to_object)
                    .ok_or(CatalogError::UnknownRelationObject)?;
                if source.signature != *b"FACP"
                    || target.signature != *b"FACS"
                    || relation.ordinal != 0
                    || fadt_facs.is_some()
                {
                    return Err(CatalogError::InvalidDsdtRelation);
                }
                fadt_facs = Some(relation.to_object);
            }
            _ => return Err(CatalogError::InvalidDsdtRelation),
        }
    }
    if objects.iter().any(|object| object.signature == *b"FACP") && fadt_dsdt.is_none() {
        return Err(CatalogError::InvalidDsdtRelation);
    }
    let facs_object_count = objects
        .iter()
        .filter(|object| object.signature == *b"FACS")
        .count();
    if facs_object_count != usize::from(fadt_facs.is_some()) {
        return Err(CatalogError::InvalidDsdtRelation);
    }
    if described_root_child_ordinals != root_child_ordinals {
        return Err(CatalogError::InvalidRootRelation);
    }
    let root_child_count = relations
        .iter()
        .filter(|relation| relation.kind == RELATION_ROOT_CHILD)
        .count();
    if (0..root_child_count).any(|ordinal| !root_child_ordinals[ordinal]) {
        return Err(CatalogError::InvalidRootRelation);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn aml_table_copy_checks_declared_length_and_aggregate_budget_transactionally() {
        let table = [0xa5; 36];
        let mut copied_bytes = 0;
        let copied = copy_aml_table_bytes(&table, table.len() as u64, &mut copied_bytes).unwrap();
        assert_eq!(copied, table);
        assert_eq!(copied_bytes, table.len() as u64);

        let mut unchanged = 7;
        assert_eq!(
            copy_aml_table_bytes(&table, (table.len() - 1) as u64, &mut unchanged),
            Err(CatalogError::InvalidTableLength)
        );
        assert_eq!(unchanged, 7);
        assert_eq!(
            copy_aml_table_bytes(&table, MAX_TABLE_BYTES + 1, &mut unchanged),
            Err(CatalogError::TableTooLarge)
        );
        assert_eq!(unchanged, 7);
        assert_eq!(
            copy_aml_table_bytes(&table, table.len() as u64, &mut (MAX_CATALOG_BYTES - 1)),
            Err(CatalogError::CatalogTooLarge)
        );
    }

    #[test]
    fn aml_catalog_validation_checks_per_table_and_total_byte_limits() {
        let mut dsdt = object(1, b"DSDT", 0);
        dsdt.length = 35;
        assert_eq!(
            validate_aml_order(&[dsdt.clone()]),
            Err(CatalogError::InvalidTableLength)
        );

        dsdt.length = MAX_TABLE_BYTES + 1;
        assert_eq!(
            validate_aml_order(&[dsdt.clone()]),
            Err(CatalogError::TableTooLarge)
        );

        dsdt.length = MAX_TABLE_BYTES;
        let mut catalog = vec![dsdt];
        catalog.extend((1..=16).map(|ordinal| {
            let mut ssdt = object(ordinal + 1, b"SSDT", ordinal);
            ssdt.length = MAX_TABLE_BYTES;
            ssdt
        }));
        let exact_limit_catalog = catalog[..=15].to_vec();
        assert_eq!(validate_aml_order(&exact_limit_catalog), Ok(()));
        assert_eq!(
            validate_aml_order(&catalog),
            Err(CatalogError::CatalogTooLarge)
        );
    }

    #[test]
    fn catalog_page_reservation_is_bounded_and_fallible_before_append() {
        let mut values = vec![1_u8, 2];
        assert_eq!(
            reserve_append(&mut values, 1, 2),
            Err(CatalogError::TooManyRecords)
        );
        assert_eq!(values, [1, 2]);

        assert_eq!(reserve_append(&mut values, 1, 3), Ok(()));
        assert!(values.capacity() >= 3);
        values.push(3);
        assert_eq!(values, [1, 2, 3]);

        let mut empty = Vec::<u8>::new();
        assert_eq!(
            reserve_append(&mut empty, usize::MAX, usize::MAX),
            Err(CatalogError::AllocationFailed)
        );
        assert!(empty.is_empty());
    }

    #[test]
    fn firmware_catalog_validation_rejects_record_counts_above_kernel_bounds() {
        let objects = vec![object(1, b"DSDT", 0); MAX_FIRMWARE_TABLE_COUNT + 1];
        assert_eq!(
            validate_aml_order(&objects),
            Err(CatalogError::TooManyRecords)
        );

        let relations = vec![
            Relation {
                from_object: 1,
                to_object: 2,
                kind: RELATION_ROOT_CHILD,
                ordinal: 0,
                firmware_version: "test-firmware-v1".to_owned(),
            };
            MAX_FIRMWARE_RELATION_COUNT + 1
        ];
        assert_eq!(
            validate_graph(&[], &relations),
            Err(CatalogError::TooManyRecords)
        );
    }

    fn object(id: u64, signature: &[u8; 4], ordinal: u64) -> Object {
        Object {
            object_id: id,
            signature: *signature,
            length: 36,
            root_ordinal: NO_ROOT_ORDINAL,
            aml_load_ordinal: ordinal,
            firmware_version: "test-firmware-v1".to_owned(),
        }
    }

    #[test]
    fn requires_dsdt_before_ssdt() {
        assert!(validate_aml_order(&[object(1, b"DSDT", 0), object(2, b"SSDT", 1)]).is_ok());
        assert_eq!(
            validate_aml_order(&[object(1, b"SSDT", 1)]),
            Err(CatalogError::MissingDsdt)
        );
    }

    #[test]
    fn rejects_ssdt_holes_and_duplicate_dsdt() {
        assert_eq!(
            validate_aml_order(&[object(1, b"DSDT", 0), object(2, b"SSDT", 2)]),
            Err(CatalogError::InvalidAmlOrder)
        );
        assert_eq!(
            validate_aml_order(&[object(1, b"DSDT", 0), object(2, b"DSDT", 0)]),
            Err(CatalogError::DuplicateObject)
        );
    }

    #[test]
    fn aml_order_accepts_the_kernel_catalog_bound_beyond_sixty_four_ssdts() {
        let mut objects = vec![object(1, b"DSDT", 0)];
        objects.extend((1..=65).map(|ordinal| object(ordinal + 1, b"SSDT", ordinal)));
        assert!(validate_aml_order(&objects).is_ok());
    }

    #[test]
    fn rejects_mixed_firmware_identity() {
        let mut second = object(2, b"SSDT", 1);
        second.firmware_version = "another-firmware-v1".to_owned();
        assert_eq!(
            validate_aml_order(&[object(1, b"DSDT", 0), second]),
            Err(CatalogError::InconsistentIdentity)
        );
    }

    #[test]
    fn graph_relations_are_opaque_and_typed() {
        let objects = [
            object(1, b"XSDT", u64::MAX),
            object(2, b"FACP", u64::MAX),
            object(3, b"DSDT", 0),
            object(4, b"FACS", u64::MAX),
        ];
        let mut objects = objects;
        objects[1].root_ordinal = 0;
        objects[2].root_ordinal = 1;
        assert!(
            validate_graph(
                &objects,
                &[
                    Relation {
                        from_object: 1,
                        to_object: 2,
                        kind: RELATION_ROOT_CHILD,
                        ordinal: 0,
                        firmware_version: "test-firmware-v1".to_owned(),
                    },
                    Relation {
                        from_object: 1,
                        to_object: 3,
                        kind: RELATION_ROOT_CHILD,
                        ordinal: 1,
                        firmware_version: "test-firmware-v1".to_owned(),
                    },
                    Relation {
                        from_object: 2,
                        to_object: 3,
                        kind: RELATION_FADT_DSDT,
                        ordinal: 0,
                        firmware_version: "test-firmware-v1".to_owned(),
                    },
                    Relation {
                        from_object: 2,
                        to_object: 4,
                        kind: RELATION_FADT_FACS,
                        ordinal: 0,
                        firmware_version: "test-firmware-v1".to_owned(),
                    },
                ]
            )
            .is_ok()
        );
        assert_eq!(
            validate_graph(
                &objects,
                &[Relation {
                    from_object: 2,
                    to_object: 0xfeed,
                    kind: RELATION_FADT_DSDT,
                    ordinal: 0,
                    firmware_version: "test-firmware-v1".to_owned(),
                }]
            ),
            Err(CatalogError::UnknownRelationObject)
        );
    }

    #[test]
    fn kernel_u32_max_sentinel_is_not_a_missing_root_child_edge() {
        let mut objects = [
            object(1, b"XSDT", NO_ROOT_ORDINAL),
            object(2, b"FACP", NO_ROOT_ORDINAL),
            object(3, b"DSDT", 0),
            object(4, b"FACS", NO_ROOT_ORDINAL),
        ];
        objects[1].root_ordinal = 0;
        assert!(
            validate_graph(
                &objects,
                &[
                    Relation {
                        from_object: 1,
                        to_object: 2,
                        kind: RELATION_ROOT_CHILD,
                        ordinal: 0,
                        firmware_version: "test-firmware-v1".to_owned(),
                    },
                    Relation {
                        from_object: 2,
                        to_object: 3,
                        kind: RELATION_FADT_DSDT,
                        ordinal: 0,
                        firmware_version: "test-firmware-v1".to_owned(),
                    },
                    Relation {
                        from_object: 2,
                        to_object: 4,
                        kind: RELATION_FADT_FACS,
                        ordinal: 0,
                        firmware_version: "test-firmware-v1".to_owned(),
                    },
                ]
            )
            .is_ok()
        );
    }

    #[test]
    fn rejects_root_child_objects_missing_their_graph_edges() {
        let mut objects = [
            object(1, b"XSDT", u64::MAX),
            object(2, b"FACP", u64::MAX),
            object(3, b"DSDT", 0),
        ];
        objects[1].root_ordinal = 0;
        objects[2].root_ordinal = 1;

        let relations = [Relation {
            from_object: 2,
            to_object: 3,
            kind: RELATION_FADT_DSDT,
            ordinal: 0,
            firmware_version: "test-firmware-v1".to_owned(),
        }];

        assert_eq!(
            validate_graph(&objects, &relations),
            Err(CatalogError::InvalidRootRelation)
        );
    }

    #[test]
    fn rejects_facs_objects_missing_their_fadt_relation() {
        let mut objects = [
            object(1, b"XSDT", NO_ROOT_ORDINAL),
            object(2, b"FACP", NO_ROOT_ORDINAL),
            object(3, b"DSDT", 0),
            object(4, b"FACS", NO_ROOT_ORDINAL),
        ];
        objects[1].root_ordinal = 0;
        objects[2].root_ordinal = 1;

        let relations = [
            Relation {
                from_object: 1,
                to_object: 2,
                kind: RELATION_ROOT_CHILD,
                ordinal: 0,
                firmware_version: "test-firmware-v1".to_owned(),
            },
            Relation {
                from_object: 1,
                to_object: 3,
                kind: RELATION_ROOT_CHILD,
                ordinal: 1,
                firmware_version: "test-firmware-v1".to_owned(),
            },
            Relation {
                from_object: 2,
                to_object: 3,
                kind: RELATION_FADT_DSDT,
                ordinal: 0,
                firmware_version: "test-firmware-v1".to_owned(),
            },
        ];

        assert_eq!(
            validate_graph(&objects, &relations),
            Err(CatalogError::InvalidDsdtRelation)
        );
    }
}
