# Architecture Overview

> System-level diagrams for the Linux kernel driver projects in this repository.

---

## 1. PCIe P2P DMA — FPGA to GPU (56% Latency Reduction)

This is the core architecture that reduced video pipeline latency from **~45ms to <20ms** on a fielded unmanned ground control system.

```
  ┌─────────────────────────────────────────────────────────────────┐
  │                     Host System (x86 / ARM)                     │
  │                                                                  │
  │   CPU                                                            │
  │   ┌────────┐                                                     │
  │   │ Driver │ pcie_dma_driver.c                                   │
  │   │        │ - BAR0 MMIO register access                        │
  │   │        │ - MSI-X IRQ affinity (CPU0=done, CPU1=error)       │
  │   │        │ - S/G descriptor ring programming                  │
  │   └───┬────┘                                                     │
  │       │ PCIe MMIO / MSI-X                                        │
  │       │                                                          │
  │   ┌───▼────────────────────────────────────────────┐            │
  │   │              PCIe Root Complex                  │            │
  │   └───────────────────┬────────────────────────────┘            │
  │                        │ PCIe Gen3 x4                            │
  └────────────────────────┼────────────────────────────────────────┘
                           │
              ┌────────────▼────────────┐
              │      PCIe Switch        │
              │  (P2P routing enabled)  │
              └──────┬──────────┬───────┘
                     │          │
         PCIe Gen3 x4│          │PCIe Gen3 x4
                     │          │
          ┌──────────▼──┐  ┌────▼──────────┐
          │    FPGA      │  │  NVIDIA GPU   │
          │  (Capture)   │  │  (Encoder)    │
          │              │  │               │
          │ Video frames │─▶│ GPU memory    │
          │ DMA → GPU    │  │ H.264 encode  │
          └──────────────┘  └───────────────┘
                  │
                  │  WITHOUT P2P (old path):
                  │  FPGA → Host RAM → GPU  (~45ms)
                  │
                  │  WITH P2P (new path):
                  └▶ FPGA → GPU directly  (<20ms) ✅ 56% faster
```

**Key driver mechanisms:**
- `DMA_CTRL_P2P_MODE` bit bypasses host memory entirely
- Descriptor `dst_addr` set to GPU BAR address (not system RAM)
- `pci_p2pdma_distance()` checked at probe to verify switch support
- IRQ completion on CPU0 (pinned) — deterministic latency at 60fps

---

## 2. V4L2 DMABUF Zero-Copy Pipeline

```
  Sensor                 Kernel                    Userspace
  ──────                 ──────                    ─────────

  ARINC-818 ──DMA──▶  vb2_buffer         ──fd──▶  GStreamer
  or CSI-2             (IOMMU mapped)              v4l2src
                            │                          │
                            │  VIDIOC_EXPBUF           │
                            │  (dmabuf fd export)      │
                            │                          ▼
                            │                     nvh264enc
                            │                     (GPU encode)
                            │                          │
                            │                          ▼
                            │                     RTSP stream
                            │                     to operator
                            │
                       ┌────┴──────────────────────────┐
                       │  cap_irq_handler()             │
                       │  → vb2_buffer_done()           │
                       │  → next buf → hw_program()    │
                       └───────────────────────────────┘

  Zero copies: sensor DMA writes directly to DMABUF
               GPU reads directly from same physical pages
               No memcpy() anywhere in the path
```

---

## 3. Heterogeneous Multi-Sensor Platform

```
  ┌──────────────────────────────────────────────────────────────┐
  │          Sensor Video Processing & Digital Map Unit           │
  │                                                               │
  │  ┌──────────┐   ARINC-818   ┌─────────────────────────────┐ │
  │  │ Sensor 1 │──────────────▶│                             │ │
  │  └──────────┘               │         FPGA                │ │
  │  ┌──────────┐   ARINC-818   │   (Frame aggregator +       │ │
  │  │ Sensor 2 │──────────────▶│    PCIe DMA engine)         │ │
  │  └──────────┘               │                             │ │
  │  ┌──────────┐   CSI-2       │                             │ │
  │  │ Sensor 3 │──────────────▶│                             │ │
  │  └──────────┘               └──────────┬──────────────────┘ │
  │                                         │                    │
  │                              PCIe Gen3 x4 (P2P DMA)         │
  │                                         │                    │
  │              ┌──────────────────────────▼──────────────┐    │
  │              │        Intel Core Ultra 7                │    │
  │              │  (Host CPU — driver, map overlay logic)  │    │
  │              └──────────────┬───────────────────────────┘    │
  │                             │                                 │
  │                    NVLink / PCIe                              │
  │                             │                                 │
  │              ┌──────────────▼───────────────────────────┐    │
  │              │         NVIDIA Jetson                     │    │
  │              │  (GPU encode + digital map overlay +      │    │
  │              │   tile-based geospatial rendering)        │    │
  │              └──────────────────────────────────────────┘    │
  └──────────────────────────────────────────────────────────────┘

  3 concurrent DMABUF streams — zero host memory copies
  IOMMU domain shared across FPGA + Intel + Jetson
```

---

## 4. Error Recovery Flow (FLR)

```
  DMA Error Detected
        │
        ▼
  pcie_dma_irq_error()  [hardirq]
        │
        │  schedule_work()
        ▼
  pcie_dma_err_work()   [workqueue / process context]
        │
        ├─▶  reg_write(DMA_CTRL_RESET)     — stop DMA engine
        │
        ├─▶  pcie_flr(pdev)                — Function-Level Reset
        │         (PCIe spec § 6.6.2)
        │
        ├─▶  pci_set_master(pdev)          — re-enable bus mastering
        │
        └─▶  reg_write(IRQ_MASK, ...)      — re-enable interrupts

  Result: sub-second automatic recovery
          vs. manual reboot (minutes) previously
```

---

## Platforms Validated

| Platform | Arch | Kernel | Use Case |
|----------|------|--------|----------|
| Custom FPGA + Intel Core Ultra 7 | x86_64 | 5.15 | Video pipeline |
| Dual-PowerPC avionics board | PowerPC | 5.10 | Display system |
| ARM Cortex-A + PCIe FPGA | ARM64 | 6.1 | Ground control |
| NVIDIA Jetson Orin NX | ARM64 | 5.15 | ML + video |
