use crate::topology::{
    PlatformCompatibleIdentifier, PlatformIdentifierKind, PlatformIdentifierValue,
};
use alloc::vec::Vec;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[allow(dead_code)] // The platform contract also supports integer compatible IDs.
pub(crate) enum DriverCompatibleId {
    Text(&'static str),
    Integer(u64),
    Pci { vendor_id: u16, device_id: u16 },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct DriverBinding {
    pub compatible_id: DriverCompatibleId,
    pub image_uri: &'static str,
    pub executable_path: &'static str,
    pub profile_rights: u32,
    pub profile_bar_mask: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum DriverMatchError {
    AmbiguousCompatibleIdentifier,
    InvalidCompatibleIdentifier,
    DuplicateCompatibleIdentifier,
    InvalidImageUri,
    InvalidExecutablePath,
    UnsupportedProfileRights,
    InvalidBarMask,
}

const DRIVER_PROFILE_RIGHTS_MASK: u32 = (1 << 3) - 1;
const DRIVER_PROFILE_BAR_MASK: u32 = (1 << 6) - 1;

fn valid_boot_module_uri(uri: &str) -> bool {
    let Some(module) = uri.strip_prefix("naos://service/boot/module/") else {
        return false;
    };
    module.len() <= 15
        && !module.is_empty()
        && module.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'-' | b'_')
        })
}

fn valid_executable_path(path: &str) -> bool {
    if path.len() > 255 || !path.starts_with('/') || path == "/" || !path.is_ascii() {
        return false;
    }

    path[1..].split('/').all(|component| {
        !component.is_empty()
            && component != "."
            && component != ".."
            && component
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    })
}

pub(crate) fn validate_driver_catalog(catalog: &[DriverBinding]) -> Result<(), DriverMatchError> {
    for (index, driver) in catalog.iter().enumerate() {
        match driver.compatible_id {
            DriverCompatibleId::Text(value)
                if value.is_empty() || value.len() > 255 || !value.is_ascii() =>
            {
                return Err(DriverMatchError::InvalidCompatibleIdentifier);
            }
            DriverCompatibleId::Pci { vendor_id, .. }
                if vendor_id == 0 || vendor_id == u16::MAX =>
            {
                return Err(DriverMatchError::InvalidCompatibleIdentifier);
            }
            DriverCompatibleId::Text(_)
            | DriverCompatibleId::Integer(_)
            | DriverCompatibleId::Pci { .. } => {}
        }

        if !valid_boot_module_uri(driver.image_uri) {
            return Err(DriverMatchError::InvalidImageUri);
        }
        if !valid_executable_path(driver.executable_path) {
            return Err(DriverMatchError::InvalidExecutablePath);
        }
        if driver.profile_rights & !DRIVER_PROFILE_RIGHTS_MASK != 0 {
            return Err(DriverMatchError::UnsupportedProfileRights);
        }
        if driver.profile_bar_mask & !DRIVER_PROFILE_BAR_MASK != 0 {
            return Err(DriverMatchError::InvalidBarMask);
        }
        if catalog[..index]
            .iter()
            .any(|prior| prior.compatible_id == driver.compatible_id)
        {
            return Err(DriverMatchError::DuplicateCompatibleIdentifier);
        }
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct DriverPlanEntry<'a> {
    pub function_index: usize,
    pub driver: &'a DriverBinding,
}

#[derive(Debug, Eq, PartialEq)]
pub(crate) struct DriverImageGroup<'a> {
    pub image_uri: &'static str,
    pub assignments: Vec<DriverPlanEntry<'a>>,
}

pub(crate) fn select_driver<'a>(
    identifiers: &[PlatformCompatibleIdentifier],
    catalog: &'a [DriverBinding],
) -> Result<Option<&'a DriverBinding>, DriverMatchError> {
    for kind in [
        PlatformIdentifierKind::Primary,
        PlatformIdentifierKind::Additional,
    ] {
        for identifier in identifiers
            .iter()
            .filter(|identifier| identifier.kind == kind)
        {
            let mut matches =
                catalog
                    .iter()
                    .filter(|driver| match (&identifier.value, driver.compatible_id) {
                        (
                            PlatformIdentifierValue::Text(value),
                            DriverCompatibleId::Text(expected),
                        ) => value.as_slice() == expected.as_bytes(),
                        (
                            PlatformIdentifierValue::Integer(value),
                            DriverCompatibleId::Integer(expected),
                        ) => *value == expected,
                        (PlatformIdentifierValue::Text(_), DriverCompatibleId::Integer(_))
                        | (PlatformIdentifierValue::Integer(_), DriverCompatibleId::Text(_))
                        | (_, DriverCompatibleId::Pci { .. }) => false,
                    });
            let Some(driver) = matches.next() else {
                continue;
            };
            if matches.next().is_some() {
                return Err(DriverMatchError::AmbiguousCompatibleIdentifier);
            }
            return Ok(Some(driver));
        }
    }
    Ok(None)
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct DriverFunctionIdentifiers<'a> {
    pub compatible_ids: &'a [PlatformCompatibleIdentifier],
    pub vendor_id: u16,
    pub device_id: u16,
}

pub(crate) fn select_driver_for_function<'a>(
    function: DriverFunctionIdentifiers<'_>,
    catalog: &'a [DriverBinding],
) -> Result<Option<&'a DriverBinding>, DriverMatchError> {
    if let Some(driver) = select_driver(function.compatible_ids, catalog)? {
        return Ok(Some(driver));
    }
    Ok(catalog.iter().find(|driver| {
        matches!(
            driver.compatible_id,
            DriverCompatibleId::Pci {
                vendor_id,
                device_id
            } if vendor_id == function.vendor_id && device_id == function.device_id
        )
    }))
}

/// Resolve the entire immutable function snapshot before the caller performs
/// any grant or process-start side effects. A later ambiguity therefore
/// cannot leave an earlier function with a partially launched driver.
pub(crate) fn plan_driver_bindings<'a>(
    functions: &[&[PlatformCompatibleIdentifier]],
    catalog: &'a [DriverBinding],
) -> Result<Vec<DriverPlanEntry<'a>>, DriverMatchError> {
    validate_driver_catalog(catalog)?;
    let mut plan = Vec::new();
    for (function_index, identifiers) in functions.iter().enumerate() {
        if let Some(driver) = select_driver(identifiers, catalog)? {
            plan.push(DriverPlanEntry {
                function_index,
                driver,
            });
        }
    }
    Ok(plan)
}

pub(crate) fn plan_driver_bindings_for_functions<'a>(
    functions: &[DriverFunctionIdentifiers<'_>],
    catalog: &'a [DriverBinding],
) -> Result<Vec<DriverPlanEntry<'a>>, DriverMatchError> {
    validate_driver_catalog(catalog)?;
    let mut plan = Vec::new();
    for (function_index, function) in functions.iter().copied().enumerate() {
        if let Some(driver) = select_driver_for_function(function, catalog)? {
            plan.push(DriverPlanEntry {
                function_index,
                driver,
            });
        }
    }
    Ok(plan)
}

/// A boot-module image locator is one-shot. Group assignments by image so the
/// manager resolves each module once and reuses its immutable bytes to prepare
/// every process that runs from that image. Group and assignment order are
/// stable by first appearance in the complete PCI scan plan.
pub(crate) fn group_driver_plan_by_image<'a>(
    plan: Vec<DriverPlanEntry<'a>>,
) -> Vec<DriverImageGroup<'a>> {
    let mut groups: Vec<DriverImageGroup<'a>> = Vec::new();
    for assignment in plan {
        let image_uri = assignment.driver.image_uri;
        if let Some(group) = groups.iter_mut().find(|group| group.image_uri == image_uri) {
            group.assignments.push(assignment);
        } else {
            groups.push(DriverImageGroup {
                image_uri,
                assignments: vec![assignment],
            });
        }
    }
    groups
}

/// Do not start any child unless every assignment in the immutable scan plan
/// has completed image loading, function validation, and capability grant.
/// A mismatch drops all deferred children as this function returns.
pub(crate) fn finish_driver_preparation<T>(
    expected_count: usize,
    prepared: Vec<T>,
) -> Result<Vec<T>, usize> {
    if prepared.len() == expected_count {
        Ok(prepared)
    } else {
        Err(prepared.len())
    }
}

#[cfg(test)]
mod tests {
    use super::{
        DriverBinding, DriverCompatibleId, DriverMatchError, finish_driver_preparation,
        plan_driver_bindings, select_driver, validate_driver_catalog,
    };
    use crate::topology::{
        PlatformCompatibleIdentifier, PlatformIdentifierKind, PlatformIdentifierValue,
    };

    #[test]
    fn selects_the_first_platform_compatible_identifier_not_catalog_order() {
        let catalog = [
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0003"),
                image_uri: "naos://service/boot/module/secondary",
                executable_path: "/secondary",
                profile_rights: 0,
                profile_bar_mask: 0,
            },
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0002"),
                image_uri: "naos://service/boot/module/primary",
                executable_path: "/primary",
                profile_rights: 1,
                profile_bar_mask: 1,
            },
        ];
        let identifiers = [
            PlatformCompatibleIdentifier {
                kind: PlatformIdentifierKind::Primary,
                value: PlatformIdentifierValue::Text(b"QEMU0002".to_vec()),
            },
            PlatformCompatibleIdentifier {
                kind: PlatformIdentifierKind::Additional,
                value: PlatformIdentifierValue::Text(b"QEMU0003".to_vec()),
            },
        ];

        assert_eq!(select_driver(&identifiers, &catalog), Ok(Some(&catalog[1])));
    }

    #[test]
    fn matches_integer_identifiers_without_coercing_them_to_strings() {
        let catalog = [DriverBinding {
            compatible_id: DriverCompatibleId::Integer(0x030a_d041),
            image_uri: "naos://service/boot/module/integer",
            executable_path: "/integer",
            profile_rights: 0,
            profile_bar_mask: 0,
        }];
        let identifiers = [PlatformCompatibleIdentifier {
            kind: PlatformIdentifierKind::Primary,
            value: PlatformIdentifierValue::Integer(0x030a_d041),
        }];

        assert_eq!(select_driver(&identifiers, &catalog), Ok(Some(&catalog[0])));
    }

    #[test]
    fn rejects_ambiguous_registry_entries_for_one_compatible_id() {
        let catalog = [
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0002"),
                image_uri: "naos://service/boot/module/first",
                executable_path: "/first",
                profile_rights: 0,
                profile_bar_mask: 0,
            },
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0002"),
                image_uri: "naos://service/boot/module/second",
                executable_path: "/second",
                profile_rights: 0,
                profile_bar_mask: 0,
            },
        ];
        let identifiers = [PlatformCompatibleIdentifier {
            kind: PlatformIdentifierKind::Primary,
            value: PlatformIdentifierValue::Text(b"QEMU0002".to_vec()),
        }];

        assert_eq!(
            select_driver(&identifiers, &catalog),
            Err(DriverMatchError::AmbiguousCompatibleIdentifier)
        );
    }

    #[test]
    fn plans_every_matching_function_in_stable_scan_order_before_launch() {
        use super::plan_driver_bindings;

        let catalog = [DriverBinding {
            compatible_id: DriverCompatibleId::Text("QEMU0002"),
            image_uri: "naos://service/boot/module/edud",
            executable_path: "/edud",
            profile_rights: 1,
            profile_bar_mask: 1,
        }];
        let first = [PlatformCompatibleIdentifier {
            kind: PlatformIdentifierKind::Primary,
            value: PlatformIdentifierValue::Text(b"QEMU0002".to_vec()),
        }];
        let unmatched = [PlatformCompatibleIdentifier {
            kind: PlatformIdentifierKind::Primary,
            value: PlatformIdentifierValue::Text(b"OTHER000".to_vec()),
        }];
        let second = [PlatformCompatibleIdentifier {
            kind: PlatformIdentifierKind::Additional,
            value: PlatformIdentifierValue::Text(b"QEMU0002".to_vec()),
        }];
        let functions: [&[PlatformCompatibleIdentifier]; 3] = [&first, &unmatched, &second];

        let plan = plan_driver_bindings(&functions, &catalog).unwrap();

        assert_eq!(plan.len(), 2);
        assert_eq!(plan[0].function_index, 0);
        assert_eq!(plan[1].function_index, 2);
        assert!(core::ptr::eq(plan[0].driver, plan[1].driver));
    }

    #[test]
    fn rejects_ambiguous_later_function_while_planning_before_any_launch() {
        use super::plan_driver_bindings;

        let catalog = [
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0002"),
                image_uri: "naos://service/boot/module/first",
                executable_path: "/first",
                profile_rights: 0,
                profile_bar_mask: 0,
            },
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0002"),
                image_uri: "naos://service/boot/module/second",
                executable_path: "/second",
                profile_rights: 0,
                profile_bar_mask: 0,
            },
        ];
        let matched = [PlatformCompatibleIdentifier {
            kind: PlatformIdentifierKind::Primary,
            value: PlatformIdentifierValue::Text(b"QEMU0002".to_vec()),
        }];
        let unmatched = [PlatformCompatibleIdentifier {
            kind: PlatformIdentifierKind::Primary,
            value: PlatformIdentifierValue::Text(b"OTHER000".to_vec()),
        }];
        let functions: [&[PlatformCompatibleIdentifier]; 2] = [&unmatched, &matched];

        assert_eq!(
            plan_driver_bindings(&functions, &catalog),
            Err(DriverMatchError::DuplicateCompatibleIdentifier)
        );
    }

    #[test]
    fn groups_planned_functions_by_one_shot_image_without_reordering_each_group() {
        use super::{group_driver_plan_by_image, plan_driver_bindings};

        let catalog = [
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0002"),
                image_uri: "naos://service/boot/module/shared",
                executable_path: "/edud",
                profile_rights: 1,
                profile_bar_mask: 1,
            },
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0004"),
                image_uri: "naos://service/boot/module/other",
                executable_path: "/other",
                profile_rights: 0,
                profile_bar_mask: 0,
            },
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0003"),
                image_uri: "naos://service/boot/module/shared",
                executable_path: "/edud",
                profile_rights: 1,
                profile_bar_mask: 1,
            },
        ];
        let identifiers = [
            [PlatformCompatibleIdentifier {
                kind: PlatformIdentifierKind::Primary,
                value: PlatformIdentifierValue::Text(b"QEMU0002".to_vec()),
            }],
            [PlatformCompatibleIdentifier {
                kind: PlatformIdentifierKind::Primary,
                value: PlatformIdentifierValue::Text(b"QEMU0004".to_vec()),
            }],
            [PlatformCompatibleIdentifier {
                kind: PlatformIdentifierKind::Primary,
                value: PlatformIdentifierValue::Text(b"QEMU0003".to_vec()),
            }],
        ];
        let functions: [&[PlatformCompatibleIdentifier]; 3] =
            [&identifiers[0], &identifiers[1], &identifiers[2]];
        let plan = plan_driver_bindings(&functions, &catalog).unwrap();

        let groups = group_driver_plan_by_image(plan);

        assert_eq!(groups.len(), 2);
        assert_eq!(groups[0].image_uri, "naos://service/boot/module/shared");
        assert_eq!(groups[0].assignments.len(), 2);
        assert_eq!(groups[0].assignments[0].function_index, 0);
        assert_eq!(groups[0].assignments[1].function_index, 2);
        assert_eq!(groups[1].image_uri, "naos://service/boot/module/other");
        assert_eq!(groups[1].assignments.len(), 1);
        assert_eq!(groups[1].assignments[0].function_index, 1);
    }

    #[test]
    fn incomplete_preparation_drops_every_deferred_child_before_any_start() {
        use std::cell::Cell;
        use std::rc::Rc;

        struct DropProbe(Rc<Cell<usize>>);

        impl Drop for DropProbe {
            fn drop(&mut self) {
                self.0.set(self.0.get() + 1);
            }
        }

        let dropped = Rc::new(Cell::new(0));
        let prepared = vec![DropProbe(dropped.clone()), DropProbe(dropped.clone())];

        let result = finish_driver_preparation(3, prepared);

        assert!(matches!(result, Err(2)));
        assert_eq!(dropped.get(), 2);
    }

    #[test]
    fn complete_preparation_releases_all_planned_children_for_start() {
        let prepared = vec![1, 2, 3];

        let result = finish_driver_preparation(3, prepared);

        assert_eq!(result, Ok(vec![1, 2, 3]));
    }

    #[test]
    fn validates_the_entire_catalog_before_inspecting_the_function_snapshot() {
        let catalog = [
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0002"),
                image_uri: "naos://service/boot/module/first",
                executable_path: "/first",
                profile_rights: 0,
                profile_bar_mask: 0,
            },
            DriverBinding {
                compatible_id: DriverCompatibleId::Text("QEMU0002"),
                image_uri: "naos://service/boot/module/second",
                executable_path: "/second",
                profile_rights: 0,
                profile_bar_mask: 0,
            },
        ];

        assert_eq!(
            validate_driver_catalog(&catalog),
            Err(DriverMatchError::DuplicateCompatibleIdentifier)
        );
        assert_eq!(
            plan_driver_bindings(&[], &catalog),
            Err(DriverMatchError::DuplicateCompatibleIdentifier)
        );
    }

    #[test]
    fn rejects_driver_entries_that_escape_the_boot_module_or_capability_profile_contract() {
        let valid = DriverBinding {
            compatible_id: DriverCompatibleId::Text("QEMU0002"),
            image_uri: "naos://service/boot/module/edud",
            executable_path: "/edud",
            profile_rights: 0b111,
            profile_bar_mask: 0b11_1111,
        };
        assert_eq!(validate_driver_catalog(&[valid]), Ok(()));

        let invalid_entries = [
            (
                DriverBinding {
                    image_uri: "naos://service/boot/module/../edud",
                    ..valid
                },
                DriverMatchError::InvalidImageUri,
            ),
            (
                DriverBinding {
                    executable_path: "/../edud",
                    ..valid
                },
                DriverMatchError::InvalidExecutablePath,
            ),
            (
                DriverBinding {
                    profile_rights: 1 << 3,
                    ..valid
                },
                DriverMatchError::UnsupportedProfileRights,
            ),
            (
                DriverBinding {
                    profile_bar_mask: 1 << 6,
                    ..valid
                },
                DriverMatchError::InvalidBarMask,
            ),
        ];

        for (entry, expected) in invalid_entries {
            assert_eq!(validate_driver_catalog(&[entry]), Err(expected));
        }
    }
}
