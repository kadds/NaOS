pub(crate) const HEALTH_DEGRADED: u32 = 1 << 0;
pub(crate) const HEALTH_AML_DEADLINE_EXCEEDED: u32 = 1 << 1;
pub(crate) const HEALTH_DISCOVERY_FAILED: u32 = 1 << 2;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct PlatformHealthInputs {
    pub(crate) pic_degraded: bool,
    pub(crate) region_registration_degraded: bool,
    pub(crate) deadline_exceeded: bool,
    pub(crate) discovery_failed: bool,
    pub(crate) negotiation_incomplete: bool,
}

pub(crate) fn compute_platform_health(inputs: PlatformHealthInputs) -> u32 {
    let degraded = inputs.pic_degraded
        || inputs.region_registration_degraded
        || inputs.deadline_exceeded
        || inputs.discovery_failed
        || inputs.negotiation_incomplete;
    let mut health = if degraded { HEALTH_DEGRADED } else { 0 };
    if inputs.deadline_exceeded {
        health |= HEALTH_AML_DEADLINE_EXCEEDED;
    }
    if inputs.discovery_failed {
        health |= HEALTH_DISCOVERY_FAILED;
    }
    health
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn failed_discovery_cannot_publish_a_healthy_empty_topology() {
        let health = compute_platform_health(PlatformHealthInputs {
            discovery_failed: true,
            ..PlatformHealthInputs::default()
        });

        assert_eq!(health & HEALTH_DEGRADED, HEALTH_DEGRADED);
        assert_eq!(health & HEALTH_DISCOVERY_FAILED, HEALTH_DISCOVERY_FAILED);
    }

    #[test]
    fn successful_no_mcfg_discovery_can_remain_healthy() {
        assert_eq!(compute_platform_health(PlatformHealthInputs::default()), 0);
    }

    #[test]
    #[test]
    fn aml_deadline_failure_sets_a_distinct_health_bit() {
        let health = compute_platform_health(PlatformHealthInputs {
            deadline_exceeded: true,
            ..PlatformHealthInputs::default()
        });

        assert_eq!(health, HEALTH_DEGRADED | HEALTH_AML_DEADLINE_EXCEEDED);
    }
}
