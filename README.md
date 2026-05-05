# linux-kernel-drivers

> Production Linux kernel drivers for PCIe, DMA, and V4L2 multimedia pipelines.
> Built from real-world defense & aerospace embedded systems experience.

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
![Language: C](https://img.shields.io/badge/language-C-blue)
![Kernel: 5.15+](https://img.shields.io/badge/kernel-5.15%2B-green)
![Platform: ARM | x86 | PowerPC](https://img.shields.io/badge/platform-ARM%20%7C%20x86%20%7C%20PowerPC-lightgrey)

---

## Author

**Arun Kumar A S** — Linux PCIe/DMA & Multimedia Driver Engineer
3+ years at [Data Patterns (India) Ltd](https://www.datapatternsindia.com/) — Defense & Aerospace | DO-178C environments

📧 arunajayakumar96@gmail.com | 📍 Trivandrum, India
🔗 [LinkedIn](https://linkedin.com/in/arun-kumar-a-s-1b49691b7) | [Portfolio](https://arun-kumar-emmo.vercel.app)

---

## Highlights

| Achievement | Detail |
|-------------|--------|
| ✂️ **56% latency reduction** | 45ms → <20ms via PCIe P2P DMA (FPGA → GPU direct) |
| 📹 **3 simultaneous streams** | Zero-copy DMABUF pipeline on heterogeneous platform |
| 🔧 **3 BSP bring-ups** | ARM, PowerPC, Intel x86 from bootloader to driver |
| 🐛 **15+ critical bugs fixed** | Kernel panics, DMA stalls, interrupt storms via ftrace/JTAG |
| ⚡ **Sub-second FLR recovery** | Automated Function-Level Reset replacing manual reboot |

---

## Repository Structure

```
linux-kernel-drivers/
│
├── pcie-dma-driver/          # PCIe Gen3 endpoint driver + scatter-gather DMA
│   ├── pcie_dma_driver.c     # ~450 lines: BAR MMIO, MSI-X, S/G DMA, AER, FLR
│   ├── Makefile
│   └── README.md
│
├── v4l2-dmabuf-pipeline/     # V4L2 capture with IOMMU-backed DMABUF export
│   ├── v4l2_dmabuf_capture.c # ~500 lines: vb2, DMABUF, zero-copy to GPU
│   ├── Makefile
│   └── README.md
│
├── upstream-patches/         # Linux kernel upstream contributions (LKML)
│   └── README.md             # Patch tracker + ready-to-send patches
│
└── docs/
    └── architecture.md       # ASCII block diagrams of all pipelines
```

---

## Projects

### 🔵 [pcie-dma-driver](./pcie-dma-driver/)

PCIe Gen3 x4 endpoint driver built for FPGA-to-GPU peer-to-peer DMA on a defense ground control system.

**Key internals:**
- BAR0 MMIO register access via `ioread32`/`iowrite32`
- MSI-X with per-CPU IRQ affinity (CPU0=completion, CPU1=error) for deterministic 60fps delivery
- Scatter-gather descriptor ring (up to 256 entries, 64MB max transfer)
- P2P DMA mode bypasses host memory — FPGA writes directly to GPU BAR
- AER (Advanced Error Reporting) with slot reset and driver resume
- FLR (Function-Level Reset) for sub-second fault recovery

```c
/* Core DMA submission */
ssize_t pcie_dma_submit(struct pcie_dma_dev *dev,
                         struct page **pages, unsigned int npages);
```

---

### 🟢 [v4l2-dmabuf-pipeline](./v4l2-dmabuf-pipeline/)

V4L2 platform driver for ARINC-818 / CSI-2 sensor capture with zero-copy DMABUF export to GPU encoders.

**Key internals:**
- Full V4L2 framework: `video_device`, `vb2_queue`, complete ioctl set
- `VB2_DMABUF` memory model — buffers exported as file descriptors
- IOMMU-backed 64-bit DMA mappings (cache-coherent on ARM and x86)
- `VIDIOC_EXPBUF` → GStreamer `v4l2src io-mode=dmabuf` → `nvh264enc`
- PM runtime with pixel clock gating

```bash
# Zero-copy GStreamer pipeline
gst-launch-1.0 v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=NV12,width=1920,height=1080 ! \
  nvh264enc ! rtph264pay ! udpsink host=192.168.1.100 port=5000
```

---

### 🟡 [upstream-patches](./upstream-patches/)

Tracking upstream Linux kernel patch submissions to LKML and subsystem mailing lists.

| Patch | Subsystem | Status |
|-------|-----------|--------|
| Fix comment typo in pcie-dpc.c | PCI/PCIe | 🟡 Ready |
| docs: Clarify P2P DMA IOMMU requirement | PCI/docs | 🟡 Ready |
| dma-buf: Add missing kerneldoc params | dma-buf | 🟡 Ready |

---

## Build All

```bash
git clone https://github.com/arunadhamz/linux-kernel-drivers.git
cd linux-kernel-drivers

# PCIe DMA driver
cd pcie-dma-driver && make && cd ..

# V4L2 DMABUF driver
cd v4l2-dmabuf-pipeline && make && cd ..
```

**Requirements:** `build-essential`, `linux-headers-$(uname -r)`

```bash
sudo apt install build-essential linux-headers-$(uname -r)
```

---

## Skills Demonstrated

```
PCIe          Gen3 x4 endpoint, BAR MMIO, MSI/MSI-X, AER, FLR, P2P DMA
DMA           Scatter-gather, IOMMU, DMABUF zero-copy, cache coherency
Multimedia    V4L2, videobuf2, GStreamer, H.264, RTSP
Kernel        Memory management, IRQ subsystem, device tree, Yocto
Debugging     ftrace, perf, JTAG, dmesg, lspci, AER reports
Platforms     ARM Cortex-A/R/M, Intel x86/x64, PowerPC, NVIDIA Jetson
```

---

## License

All drivers in this repository are licensed under **GPL-2.0-only**, consistent with the Linux kernel license.

See [SPDX: GPL-2.0-only](https://spdx.org/licenses/GPL-2.0-only.html)
