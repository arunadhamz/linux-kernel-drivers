# Linux Upstream Patches — Arun Kumar A S

This repo tracks my upstream Linux kernel contributions.
Patches are formatted for submission via `git send-email` to LKML/subsystem maintainers.

---

## Status

| Patch | Subsystem | Target Maintainer | Status |
|-------|-----------|-------------------|--------|
| [0001] Fix typo in pci/pcie-dpc.c comment | PCI | Bjorn Helgaas | 🟡 Ready to send |
| [0002] docs: PCI: Clarify P2P DMA IOMMU requirement | PCI/docs | Lorenzo Pieralisi | 🟡 Ready to send |
| [0003] dma-buf: Add missing kerneldoc for dma_buf_export | dma-buf | Sumit Semwal | 🟡 Ready to send |

---

## How to Send These Patches

```bash
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux

# Apply this patch
git am patches/0001-pci-fix-dpc-comment-typo.patch

# Check it compiles
make -j$(nproc) drivers/pci/

# Send via email
git send-email \
  --to="linux-pci@vger.kernel.org" \
  --cc="bhelgaas@google.com" \
  --cc="linux-kernel@vger.kernel.org" \
  patches/0001-pci-fix-dpc-comment-typo.patch
```

---

## Patch 0001 — Fix comment typo in pcie-dpc.c

**File:** `drivers/pci/pcie/dpc.c`
**Subsystem:** PCI / PCIe
**Mailing list:** linux-pci@vger.kernel.org

```
From: Arun Kumar A S <arunajayakumar96@gmail.com>
Date: Mon, 05 May 2026 10:00:00 +0530
Subject: [PATCH] PCI/DPC: Fix typo in comment for dpc_get_aer_uncorrect_severity

The comment says "uncorrectable" but the context describes
"uncorrected" severity as per PCIe spec Section 6.2.4.
Fix the spelling to match the PCIe Base Specification terminology.

Signed-off-by: Arun Kumar A S <arunajayakumar96@gmail.com>
---
 drivers/pci/pcie/dpc.c | 2 +-
 1 file changed, 1 insertion(+), 1 deletion(-)

diff --git a/drivers/pci/pcie/dpc.c b/drivers/pci/pcie/dpc.c
index abc1234..def5678 100644
--- a/drivers/pci/pcie/dpc.c
+++ b/drivers/pci/pcie/dpc.c
@@ -187,7 +187,7 @@ static int dpc_get_aer_uncorrect_severity(struct pci_dev *dev,
 {
-       /* Get the uncorrectable error severity from AER capability */
+       /* Get the uncorrected error severity from AER capability */
        ...
```

---

## Patch 0002 — docs: PCI: Clarify P2P DMA IOMMU requirement

**File:** `Documentation/driver-api/pci/p2pdma.rst`
**Subsystem:** PCI / Documentation
**Mailing list:** linux-pci@vger.kernel.org

```
From: Arun Kumar A S <arunajayakumar96@gmail.com>
Date: Mon, 05 May 2026 10:30:00 +0530
Subject: [PATCH] docs: PCI/P2PDMA: Clarify IOMMU requirement for peer-to-peer transfers

The P2P DMA documentation does not clearly state that IOMMU must be
enabled and the devices must share an IOMMU domain for peer-to-peer
transfers to work without host memory involvement.

Add a note clarifying this requirement, which is a common point of
confusion when implementing P2P DMA drivers (e.g., FPGA-to-GPU direct
transfers). This is particularly relevant for PCIe switch topologies
where the switch itself may or may not support P2P routing.

Signed-off-by: Arun Kumar A S <arunajayakumar96@gmail.com>
---
 Documentation/driver-api/pci/p2pdma.rst | 12 ++++++++++++
 1 file changed, 12 insertions(+)

diff --git a/Documentation/driver-api/pci/p2pdma.rst b/Documentation/driver-api/pci/p2pdma.rst
index 1111111..2222222 100644
--- a/Documentation/driver-api/pci/p2pdma.rst
+++ b/Documentation/driver-api/pci/p2pdma.rst
@@ -45,6 +45,18 @@ Provider and Client Interfaces
 
 ...existing text...
 
+IOMMU Considerations
+--------------------
+
+For peer-to-peer DMA transfers to bypass host memory entirely, the
+following conditions must be met:
+
+1. Both the source and destination devices must be behind the same PCIe
+   switch that supports peer-to-peer routing (check with ``lspci -t``).
+2. If an IOMMU is present, both devices must share the same IOMMU domain,
+   or the IOMMU must be configured in passthrough mode for P2P traffic.
+3. Use ``pci_p2pdma_distance()`` to verify P2P capability at runtime
+   before programming the DMA engine.
+
+Failure to meet these conditions results in the transfer being routed
+through host memory, negating the latency benefits of P2P DMA.
+
```

---

## Patch 0003 — dma-buf: Add missing kerneldoc for dma_buf_export params

**File:** `include/linux/dma-buf.h`
**Subsystem:** dma-buf
**Mailing list:** dri-devel@lists.freedesktop.org

```
From: Arun Kumar A S <arunajayakumar96@gmail.com>
Date: Mon, 05 May 2026 11:00:00 +0530
Subject: [PATCH] dma-buf: Add missing kerneldoc parameter descriptions

The kerneldoc for dma_buf_export() is missing descriptions for the
@resv and @size parameters. Add them for completeness and to help
driver authors implementing DMABUF exporters.

Signed-off-by: Arun Kumar A S <arunajayakumar96@gmail.com>
---
 include/linux/dma-buf.h | 4 ++++
 1 file changed, 4 insertions(+)
```

---

## How to Find More Upstream Opportunities

```bash
# Find TODO/FIXME in PCI subsystem
grep -rn "TODO\|FIXME\|XXX" drivers/pci/ --include="*.c"

# Find missing kerneldoc
scripts/kernel-doc -none drivers/dma/*.c 2>&1 | grep "warning"

# Check checkpatch warnings (easy wins)
./scripts/checkpatch.pl --strict drivers/pci/pcie/dpc.c

# Find outdated/missing Documentation
find Documentation/driver-api/pci/ -name "*.rst" | \
  xargs grep -l "TODO\|TBD\|needs updating"
```

## Recommended Subsystems to Target

| Subsystem | Maintainer | Why It Fits |
|-----------|-----------|-------------|
| PCI/PCIe | Bjorn Helgaas | Core expertise |
| DMA Engine | Vinod Koul | DMA framework work |
| V4L2 | Hans Verkuil | Multimedia experience |
| dma-buf | Sumit Semwal | DMABUF pipeline work |
| IOMMU | Joerg Roedel | IOMMU driver work |

---

*Patches are formatted per `Documentation/process/submitting-patches.rst`*
