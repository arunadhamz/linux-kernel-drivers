# linux-pcie-dma-driver

> Production-style PCIe Gen3 endpoint driver with scatter-gather DMA, MSI-X, AER, and FLR recovery.

Derived from real-world experience building PCIe drivers for defense/aerospace FPGA platforms. This driver achieved **56% video latency reduction** (45ms → <20ms) in a fielded unmanned ground control system by enabling peer-to-peer DMA between FPGA capture hardware and GPU memory.

## Features

- **BAR0 MMIO** — register access via `ioread32`/`iowrite32` with proper barriers
- **MSI-X interrupts** — up to 8 vectors with per-CPU IRQ affinity tuning for deterministic latency
- **Scatter-gather DMA** — hardware descriptor ring supporting up to 256 SG entries and 64MB transfers
- **P2P DMA** — peer-to-peer mode bypasses host memory (FPGA → GPU direct)
- **AER support** — Advanced Error Reporting with automatic slot reset and recovery
- **FLR recovery** — Function-Level Reset reduces MTTR from manual reboot to sub-second

## Architecture

```
User space
    │  pcie_dma_submit(pages[], npages)
    ▼
┌─────────────────────────────────────────────┐
│            pcie_dma_driver.c                │
│                                             │
│  sg_init_table → dma_map_sg                 │
│       │                                     │
│  build_descriptors → descriptor ring        │
│       │                                     │
│  reg_write(REG_DMA_DESC_LO/HI/CNT)          │
│  reg_write(DMA_CTRL_SG | IRQ_EN | START)    │
│       │                                     │
│  wait_for_completion_timeout(5000ms)        │
│       ▲                                     │
│  pcie_dma_irq_done() [MSI-X vector 0]       │
│  → complete(&xfer->completion)              │
└─────────────────────────────────────────────┘
    │
    ▼  PCIe Gen3 x4
┌──────────┐    ┌──────────┐
│  FPGA    │───▶│   GPU    │  (P2P DMA, no host memory copy)
│  Capture │    │  Memory  │
└──────────┘    └──────────┘
```

## Register Map (BAR0)

| Offset | Register | Description |
|--------|----------|-------------|
| 0x0000 | DMA_CTRL | Start, reset, mode select |
| 0x0004 | DMA_STATUS | Done, error, busy flags |
| 0x001C/20 | DMA_DESC_LO/HI | Descriptor ring base address |
| 0x0024 | DMA_DESC_CNT | Number of descriptors |
| 0x0040 | IRQ_STATUS | Interrupt status |
| 0x0044 | IRQ_MASK | Interrupt enable mask |

## Build

```bash
# Build against running kernel
make

# Build against specific kernel tree
make KDIR=/path/to/linux

# Install
sudo make install
```

## Load / Test

```bash
sudo insmod pcie_dma_driver.ko
dmesg | grep pcie_dma         # Verify probe success
lspci -vvv | grep -A5 "10ee:9038"  # Check MSI-X capability
```

## Key Implementation Notes

**IRQ Affinity**: DMA completion IRQ pinned to CPU0, error IRQ to CPU1. This prevents completion latency jitter from cross-CPU migrations under load.

**Descriptor Coherency**: Descriptors allocated via `dma_alloc_coherent()` — ensures CPU writes are visible to device without explicit cache flush on both x86 and ARM.

**Error Recovery**: AER-driven slot reset re-enables bus mastering and restores interrupt masks. FLR in `pcie_dma_err_work` handles soft errors without full slot reset.

## Kernel Compatibility

| Kernel | Status |
|--------|--------|
| 5.15 LTS | ✅ Tested |
| 6.1 LTS  | ✅ Tested |
| 6.6 LTS  | ✅ Tested |

## Author

Arun Kumar A S — [arunajayakumar96@gmail.com](mailto:arunajayakumar96@gmail.com)

Linux PCIe/DMA & Multimedia Driver Engineer at Data Patterns (India) Ltd

## License

GPL-2.0-only
