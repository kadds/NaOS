pub(crate) const MAX_PCI_OSC_PATH_BYTES: usize = 255;

/// Stack-sized transport for an AML root path. The request queue must not
/// allocate a second owned String before the executor's work-unit meter runs.
#[derive(Clone, Copy)]
pub(crate) struct FixedPciOscPath {
    bytes: [u8; MAX_PCI_OSC_PATH_BYTES],
    length: u8,
}

impl FixedPciOscPath {
    pub(crate) fn try_from_path(path: &str) -> Option<Self> {
        let source = path.as_bytes();
        if source.len() > MAX_PCI_OSC_PATH_BYTES
            || !source.starts_with(b"\\")
            || source.contains(&0)
        {
            return None;
        }

        let mut bytes = [0; MAX_PCI_OSC_PATH_BYTES];
        bytes[..source.len()].copy_from_slice(source);
        Some(Self {
            bytes,
            length: source.len() as u8,
        })
    }

    pub(crate) fn as_str(&self) -> &str {
        core::str::from_utf8(&self.bytes[..usize::from(self.length)]).unwrap_or("")
    }
}

#[cfg(test)]
mod tests {
    use super::{FixedPciOscPath, MAX_PCI_OSC_PATH_BYTES};

    #[test]
    fn fixed_pci_osc_path_is_bounded_and_preserves_bytes() {
        let path = FixedPciOscPath::try_from_path("\\_SB_.PCI0").unwrap();
        assert_eq!(path.as_str(), "\\_SB_.PCI0");
        assert_eq!(
            core::mem::size_of::<FixedPciOscPath>(),
            MAX_PCI_OSC_PATH_BYTES + 1
        );

        let mut maximum_path = String::from("\\");
        maximum_path.push_str(&"A".repeat(MAX_PCI_OSC_PATH_BYTES - 1));
        assert!(FixedPciOscPath::try_from_path(&maximum_path).is_some());

        maximum_path.push('A');
        assert!(FixedPciOscPath::try_from_path(&maximum_path).is_none());
    }
}
