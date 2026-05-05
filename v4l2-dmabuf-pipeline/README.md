# v4l2-dmabuf-pipeline

> Zero-copy V4L2 video capture driver with IOMMU-backed DMABUF pipeline.

Implements a production-style V4L2 platform driver that captures video frames directly into GPU-shareable DMABUF buffers — eliminating all host memory copies between sensor capture and GPU encode/display.

Used as the foundation for a **3-stream simultaneous video pipeline** on a heterogeneous FPGA + Intel Core Ultra 7 + NVIDIA Jetson defense platform.

## Features

- **V4L2 framework** — full `video_device`, `vb2_queue`, ioctl ops
- **DMABUF memory model** — `VB2_DMABUF` export via `VIDIOC_EXPBUF`
- **IOMMU-backed DMA** — 64-bit coherent mappings, cache-coherent on ARM and x86
- **Zero-copy pipeline** — buffer fd exported directly to GStreamer/FFmpeg/GPU encoder
- **Formats** — YUYV, NV12, RGB24, BGR32 (up to 4K/2160p)
- **Overflow detection** — rate-limited `dev_warn` on frame drop
- **PM runtime** — clock gating when not streaming

## Pipeline Architecture

```
┌──────────────────────────────────────────────────────────┐
│                    Sensor Hardware                        │
│            (ARINC-818 / MIPI CSI-2)                      │
└────────────────────┬─────────────────────────────────────┘
                     │ DMA (IOMMU-mapped)
                     ▼
┌──────────────────────────────────────────────────────────┐
│              v4l2_dmabuf_capture.c                        │
│                                                           │
│  vb2_queue (VB2_DMABUF)                                   │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐                    │
│  │ buf[0]  │ │ buf[1]  │ │ buf[2]  │  (min 3 buffers)   │
│  │dma_addr │ │dma_addr │ │dma_addr │                    │
│  └────┬────┘ └─────────┘ └─────────┘                    │
│       │ cap_irq_handler → vb2_buffer_done()              │
└───────┼──────────────────────────────────────────────────┘
        │ DMABUF fd (zero-copy)
        ▼
┌───────────────────────┐    ┌──────────────────────┐
│  GStreamer Pipeline    │    │   GPU H.264 Encoder  │
│  v4l2src ! dmabuf     │───▶│   (NVIDIA nvenc /    │
│                       │    │    AMD VCE)          │
└───────────────────────┘    └──────────────────────┘
```

## Supported Formats

| fourcc | Description | BPP |
|--------|-------------|-----|
| YUYV | 4:2:2 packed | 16 |
| NV12 | 4:2:0 semi-planar | 12 |
| RGB24 | RGB 888 | 24 |
| BGR32 | BGR 8888 | 32 |

## Build

```bash
make
sudo insmod v4l2_dmabuf_capture.ko
dmesg | grep v4l2_dmabuf
```

## GStreamer Zero-Copy Usage

```bash
# Capture → H.264 encode → RTSP stream (zero-copy)
gst-launch-1.0 \
  v4l2src device=/dev/video0 io-mode=dmabuf ! \
  video/x-raw,format=NV12,width=1920,height=1080,framerate=60/1 ! \
  nvh264enc ! \
  rtph264pay ! \
  udpsink host=192.168.1.100 port=5000
```

## Kernel Compatibility

| Kernel | Status |
|--------|--------|
| 5.15 LTS | ✅ |
| 6.1 LTS  | ✅ |
| 6.6 LTS  | ✅ |

## Author

Arun Kumar A S — [arunajayakumar96@gmail.com](mailto:arunajayakumar96@gmail.com)
Linux PCIe/DMA & Multimedia Driver Engineer | Data Patterns (India) Ltd

## License

GPL-2.0-only
