// SPDX-License-Identifier: GPL-2.0-only
/*
 * v4l2_dmabuf_capture.c — V4L2 Video Capture Driver with IOMMU-backed DMABUF
 *
 * Implements a zero-copy video capture pipeline using:
 *   - V4L2 framework (video_device, vb2_queue)
 *   - Videobuf2 with DMABUF memory model
 *   - IOMMU-backed cache-coherent DMA mappings
 *   - ARINC-818 / CSI-2 sensor input abstraction
 *   - GPU-shareable DMABUF export for zero-copy to display/encoder
 *
 * Pipeline:
 *   Sensor (ARINC-818/CSI-2) → DMA → vb2 buffer (DMABUF)
 *                                           ↓ (zero-copy fd export)
 *                              GStreamer / FFmpeg / GPU encoder
 *
 * Author: Arun Kumar A S <arunajayakumar96@gmail.com>
 * Kernel: 5.15+
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-dma-sg.h>

#define DRV_NAME        "v4l2_dmabuf_cap"
#define DRV_VERSION     "1.0.0"

#define MIN_BUFFERS     3
#define MAX_BUFFERS     32
#define MAX_WIDTH       3840
#define MAX_HEIGHT      2160

/* ── Hardware Register Map ──────────────────────────────────────────── */
#define REG_CAP_CTRL        0x000   /* Capture control                  */
#define REG_CAP_STATUS      0x004   /* Capture status                   */
#define REG_FRAME_ADDR_LO   0x008   /* Current frame DMA address low    */
#define REG_FRAME_ADDR_HI   0x00C   /* Current frame DMA address high   */
#define REG_FRAME_STRIDE    0x010   /* Line stride in bytes             */
#define REG_FRAME_WIDTH     0x014   /* Frame width in pixels            */
#define REG_FRAME_HEIGHT    0x018   /* Frame height in lines            */
#define REG_IRQ_STATUS      0x040
#define REG_IRQ_MASK        0x044
#define REG_IRQ_CLEAR       0x048

#define CAP_CTRL_ENABLE     BIT(0)
#define CAP_CTRL_RESET      BIT(1)
#define CAP_IRQ_FRAME_DONE  BIT(0)
#define CAP_IRQ_OVERFLOW    BIT(1)

/**
 * struct cap_fmt - Supported pixel format descriptor
 */
struct cap_fmt {
    u32         fourcc;
    u32         depth;          /* bits per pixel */
    const char *name;
};

static const struct cap_fmt supported_fmts[] = {
    { V4L2_PIX_FMT_YUYV,   16, "YUYV 4:2:2" },
    { V4L2_PIX_FMT_NV12,   12, "NV12 4:2:0" },
    { V4L2_PIX_FMT_RGB24,  24, "RGB 888"    },
    { V4L2_PIX_FMT_BGR32,  32, "BGR 8888"   },
};

/**
 * struct cap_buffer - Per-frame videobuf2 buffer
 * @vb:        Embedded vb2_v4l2_buffer (must be first)
 * @dma_addr:  Bus address programmed into hardware
 * @list:      Entry in active queue
 */
struct cap_buffer {
    struct vb2_v4l2_buffer  vb;
    dma_addr_t              dma_addr;
    struct list_head        list;
};

/**
 * struct cap_dev - Per-device driver state
 */
struct cap_dev {
    /* V4L2 core objects */
    struct v4l2_device      v4l2_dev;
    struct video_device     vdev;
    struct v4l2_ctrl_handler ctrl_handler;

    /* Videobuf2 */
    struct vb2_queue        queue;
    struct mutex            lock;       /* Serializes V4L2 ioctls       */
    spinlock_t              buf_lock;   /* Protects active_bufs list    */
    struct list_head        active_bufs;

    /* Current format */
    struct v4l2_pix_format  fmt;
    const struct cap_fmt   *cap_fmt;

    /* Hardware */
    void __iomem           *base;
    int                     irq;
    struct clk             *clk;

    /* State */
    bool                    streaming;
    u64                     frame_count;
    u64                     overflow_count;
};

/* ── Format Helpers ─────────────────────────────────────────────────── */

static const struct cap_fmt *find_fmt(u32 fourcc)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(supported_fmts); i++)
        if (supported_fmts[i].fourcc == fourcc)
            return &supported_fmts[i];
    return NULL;
}

static void cap_fill_default_fmt(struct cap_dev *dev)
{
    dev->cap_fmt = &supported_fmts[0]; /* YUYV default */
    dev->fmt.width       = 1920;
    dev->fmt.height      = 1080;
    dev->fmt.pixelformat = dev->cap_fmt->fourcc;
    dev->fmt.field       = V4L2_FIELD_NONE;
    dev->fmt.colorspace  = V4L2_COLORSPACE_REC709;
    dev->fmt.bytesperline =
        dev->fmt.width * dev->cap_fmt->depth / 8;
    dev->fmt.sizeimage   =
        dev->fmt.bytesperline * dev->fmt.height;
}

/* ── Hardware Control ───────────────────────────────────────────────── */

static inline u32 hw_read(struct cap_dev *dev, u32 off)
{
    return ioread32(dev->base + off);
}

static inline void hw_write(struct cap_dev *dev, u32 off, u32 val)
{
    iowrite32(val, dev->base + off);
}

static void hw_program_buffer(struct cap_dev *dev, dma_addr_t addr)
{
    hw_write(dev, REG_FRAME_ADDR_LO, lower_32_bits(addr));
    hw_write(dev, REG_FRAME_ADDR_HI, upper_32_bits(addr));
}

static void hw_configure(struct cap_dev *dev)
{
    hw_write(dev, REG_FRAME_WIDTH,  dev->fmt.width);
    hw_write(dev, REG_FRAME_HEIGHT, dev->fmt.height);
    hw_write(dev, REG_FRAME_STRIDE, dev->fmt.bytesperline);
    hw_write(dev, REG_IRQ_MASK, CAP_IRQ_FRAME_DONE | CAP_IRQ_OVERFLOW);
}

/* ── Interrupt Handler ──────────────────────────────────────────────── */

static irqreturn_t cap_irq_handler(int irq, void *data)
{
    struct cap_dev *dev = data;
    struct cap_buffer *buf;
    u32 status;
    unsigned long flags;

    status = hw_read(dev, REG_IRQ_STATUS);
    hw_write(dev, REG_IRQ_CLEAR, status);

    if (status & CAP_IRQ_OVERFLOW) {
        dev->overflow_count++;
        dev_warn_ratelimited(&dev->vdev.dev,
                             "Frame overflow! count=%llu\n",
                             dev->overflow_count);
    }

    if (!(status & CAP_IRQ_FRAME_DONE))
        return IRQ_HANDLED;

    spin_lock_irqsave(&dev->buf_lock, flags);

    if (list_empty(&dev->active_bufs)) {
        spin_unlock_irqrestore(&dev->buf_lock, flags);
        return IRQ_HANDLED;
    }

    /* Retire the completed buffer */
    buf = list_first_entry(&dev->active_bufs, struct cap_buffer, list);
    list_del(&buf->list);

    buf->vb.vb2_buf.timestamp = ktime_get_ns();
    buf->vb.sequence = dev->frame_count++;
    buf->vb.field = V4L2_FIELD_NONE;

    /* Queue next buffer to hardware before signalling done */
    if (!list_empty(&dev->active_bufs)) {
        struct cap_buffer *next =
            list_first_entry(&dev->active_bufs,
                             struct cap_buffer, list);
        hw_program_buffer(dev, next->dma_addr);
    }

    spin_unlock_irqrestore(&dev->buf_lock, flags);

    /* Mark buffer done — DMABUF fd becomes readable to userspace */
    vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

    return IRQ_HANDLED;
}

/* ── Videobuf2 Operations ───────────────────────────────────────────── */

static int cap_queue_setup(struct vb2_queue *q,
                            unsigned int *nbuffers,
                            unsigned int *nplanes,
                            unsigned int sizes[],
                            struct device *alloc_devs[])
{
    struct cap_dev *dev = vb2_get_drv_priv(q);

    *nbuffers = clamp(*nbuffers, MIN_BUFFERS, MAX_BUFFERS);

    if (*nplanes) {
        if (sizes[0] < dev->fmt.sizeimage)
            return -EINVAL;
        return 0;
    }

    *nplanes = 1;
    sizes[0] = dev->fmt.sizeimage;

    dev_dbg(&dev->vdev.dev,
            "queue_setup: %u buffers, size=%u bytes (DMABUF)\n",
            *nbuffers, sizes[0]);
    return 0;
}

static int cap_buf_prepare(struct vb2_buffer *vb)
{
    struct cap_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
    struct cap_buffer *buf =
        container_of(to_vb2_v4l2_buffer(vb), struct cap_buffer, vb);

    if (vb2_plane_size(vb, 0) < dev->fmt.sizeimage) {
        dev_err(&dev->vdev.dev,
                "Buffer too small: %lu < %u\n",
                vb2_plane_size(vb, 0), dev->fmt.sizeimage);
        return -EINVAL;
    }

    vb2_set_plane_payload(vb, 0, dev->fmt.sizeimage);

    /* Get IOMMU-mapped DMA address for this DMABUF */
    buf->dma_addr = vb2_dma_contig_plane_dma_addr(vb, 0);

    return 0;
}

static void cap_buf_queue(struct vb2_buffer *vb)
{
    struct cap_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
    struct cap_buffer *buf =
        container_of(to_vb2_v4l2_buffer(vb), struct cap_buffer, vb);
    unsigned long flags;
    bool was_empty;

    spin_lock_irqsave(&dev->buf_lock, flags);
    was_empty = list_empty(&dev->active_bufs);
    list_add_tail(&buf->list, &dev->active_bufs);

    /* If hardware was idle, immediately program this buffer */
    if (was_empty && dev->streaming)
        hw_program_buffer(dev, buf->dma_addr);

    spin_unlock_irqrestore(&dev->buf_lock, flags);
}

static int cap_start_streaming(struct vb2_queue *q, unsigned int count)
{
    struct cap_dev *dev = vb2_get_drv_priv(q);
    struct cap_buffer *buf;
    unsigned long flags;
    int ret;

    ret = pm_runtime_get_sync(&dev->vdev.dev);
    if (ret < 0)
        return ret;

    hw_configure(dev);

    spin_lock_irqsave(&dev->buf_lock, flags);
    dev->streaming = true;
    if (!list_empty(&dev->active_bufs)) {
        buf = list_first_entry(&dev->active_bufs,
                               struct cap_buffer, list);
        hw_program_buffer(dev, buf->dma_addr);
    }
    spin_unlock_irqrestore(&dev->buf_lock, flags);

    hw_write(dev, REG_CAP_CTRL, CAP_CTRL_ENABLE);

    dev_info(&dev->vdev.dev,
             "Streaming started: %ux%u %s (DMABUF zero-copy)\n",
             dev->fmt.width, dev->fmt.height, dev->cap_fmt->name);
    return 0;
}

static void cap_stop_streaming(struct vb2_queue *q)
{
    struct cap_dev *dev = vb2_get_drv_priv(q);
    struct cap_buffer *buf;
    unsigned long flags;

    hw_write(dev, REG_CAP_CTRL, 0);  /* Disable capture */
    hw_write(dev, REG_IRQ_MASK, 0);

    spin_lock_irqsave(&dev->buf_lock, flags);
    dev->streaming = false;

    /* Return all queued buffers to vb2 */
    while (!list_empty(&dev->active_bufs)) {
        buf = list_first_entry(&dev->active_bufs,
                               struct cap_buffer, list);
        list_del(&buf->list);
        vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
    }
    spin_unlock_irqrestore(&dev->buf_lock, flags);

    pm_runtime_put(&dev->vdev.dev);

    dev_info(&dev->vdev.dev,
             "Streaming stopped: frames=%llu overflows=%llu\n",
             dev->frame_count, dev->overflow_count);
}

static const struct vb2_ops cap_vb2_ops = {
    .queue_setup        = cap_queue_setup,
    .buf_prepare        = cap_buf_prepare,
    .buf_queue          = cap_buf_queue,
    .start_streaming    = cap_start_streaming,
    .stop_streaming     = cap_stop_streaming,
    .wait_prepare       = vb2_ops_wait_prepare,
    .wait_finish        = vb2_ops_wait_finish,
};

/* ── V4L2 IOCTLs ────────────────────────────────────────────────────── */

static int cap_querycap(struct file *file, void *priv,
                         struct v4l2_capability *cap)
{
    strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
    strscpy(cap->card, "DMABUF Video Capture", sizeof(cap->card));
    cap->capabilities = V4L2_CAP_VIDEO_CAPTURE |
                        V4L2_CAP_STREAMING |
                        V4L2_CAP_DEVICE_CAPS;
    cap->device_caps  = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    return 0;
}

static int cap_enum_fmt(struct file *file, void *priv,
                         struct v4l2_fmtdesc *f)
{
    if (f->index >= ARRAY_SIZE(supported_fmts))
        return -EINVAL;

    f->pixelformat = supported_fmts[f->index].fourcc;
    strscpy(f->description, supported_fmts[f->index].name,
            sizeof(f->description));
    return 0;
}

static int cap_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct cap_dev *dev = video_drvdata(file);

    f->fmt.pix = dev->fmt;
    return 0;
}

static int cap_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct v4l2_pix_format *pix = &f->fmt.pix;
    const struct cap_fmt *cfmt;

    cfmt = find_fmt(pix->pixelformat);
    if (!cfmt)
        cfmt = &supported_fmts[0];

    pix->pixelformat  = cfmt->fourcc;
    pix->width        = clamp(pix->width,  64U, (u32)MAX_WIDTH);
    pix->height       = clamp(pix->height, 64U, (u32)MAX_HEIGHT);
    pix->field        = V4L2_FIELD_NONE;
    pix->colorspace   = V4L2_COLORSPACE_REC709;
    pix->bytesperline = pix->width * cfmt->depth / 8;
    pix->sizeimage    = pix->bytesperline * pix->height;
    pix->priv         = 0;
    return 0;
}

static int cap_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
    struct cap_dev *dev = video_drvdata(file);
    int ret;

    if (vb2_is_busy(&dev->queue))
        return -EBUSY;

    ret = cap_try_fmt(file, priv, f);
    if (ret)
        return ret;

    dev->cap_fmt = find_fmt(f->fmt.pix.pixelformat);
    dev->fmt     = f->fmt.pix;
    return 0;
}

static const struct v4l2_ioctl_ops cap_ioctl_ops = {
    .vidioc_querycap        = cap_querycap,
    .vidioc_enum_fmt_vid_cap = cap_enum_fmt,
    .vidioc_g_fmt_vid_cap   = cap_g_fmt,
    .vidioc_try_fmt_vid_cap  = cap_try_fmt,
    .vidioc_s_fmt_vid_cap   = cap_s_fmt,
    .vidioc_reqbufs         = vb2_ioctl_reqbufs,
    .vidioc_querybuf        = vb2_ioctl_querybuf,
    .vidioc_qbuf            = vb2_ioctl_qbuf,
    .vidioc_dqbuf           = vb2_ioctl_dqbuf,
    .vidioc_expbuf          = vb2_ioctl_expbuf,  /* DMABUF export fd */
    .vidioc_streamon        = vb2_ioctl_streamon,
    .vidioc_streamoff       = vb2_ioctl_streamoff,
    .vidioc_subscribe_event  = v4l2_ctrl_subscribe_event,
    .vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations cap_fops = {
    .owner          = THIS_MODULE,
    .open           = v4l2_fh_open,
    .release        = vb2_fop_release,
    .read           = vb2_fop_read,
    .poll           = vb2_fop_poll,
    .unlocked_ioctl = video_ioctl2,
    .mmap           = vb2_fop_mmap,
};

/* ── Platform Driver Probe ──────────────────────────────────────────── */

static int cap_probe(struct platform_device *pdev)
{
    struct cap_dev *dev;
    struct resource *res;
    int ret;

    dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    mutex_init(&dev->lock);
    spin_lock_init(&dev->buf_lock);
    INIT_LIST_HEAD(&dev->active_bufs);
    platform_set_drvdata(pdev, dev);

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    dev->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(dev->base))
        return PTR_ERR(dev->base);

    dev->irq = platform_get_irq(pdev, 0);
    if (dev->irq < 0)
        return dev->irq;

    ret = devm_request_irq(&pdev->dev, dev->irq, cap_irq_handler,
                            0, DRV_NAME, dev);
    if (ret)
        return ret;

    dev->clk = devm_clk_get(&pdev->dev, "pixel");
    if (!IS_ERR(dev->clk))
        clk_prepare_enable(dev->clk);

    /* Set up 64-bit IOMMU-backed DMA */
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret)
        return ret;

    /* V4L2 device */
    ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
    if (ret)
        return ret;

    /* Videobuf2 queue — DMABUF memory model for zero-copy */
    dev->queue.type             = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dev->queue.io_modes         = VB2_DMABUF | VB2_MMAP | VB2_READ;
    dev->queue.drv_priv         = dev;
    dev->queue.buf_struct_size  = sizeof(struct cap_buffer);
    dev->queue.ops              = &cap_vb2_ops;
    dev->queue.mem_ops          = &vb2_dma_contig_memops;
    dev->queue.timestamp_flags  = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    dev->queue.lock             = &dev->lock;
    dev->queue.min_buffers_needed = MIN_BUFFERS;

    ret = vb2_queue_init(&dev->queue);
    if (ret)
        goto err_v4l2;

    /* Video device */
    strscpy(dev->vdev.name, DRV_NAME, sizeof(dev->vdev.name));
    dev->vdev.v4l2_dev  = &dev->v4l2_dev;
    dev->vdev.fops      = &cap_fops;
    dev->vdev.ioctl_ops = &cap_ioctl_ops;
    dev->vdev.release   = video_device_release_empty;
    dev->vdev.queue     = &dev->queue;
    dev->vdev.lock      = &dev->lock;
    dev->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    video_set_drvdata(&dev->vdev, dev);

    ret = video_register_device(&dev->vdev, VFL_TYPE_VIDEO, -1);
    if (ret)
        goto err_v4l2;

    cap_fill_default_fmt(dev);
    pm_runtime_enable(&pdev->dev);

    dev_info(&pdev->dev,
             "%s registered as /dev/video%d (DMABUF zero-copy)\n",
             DRV_NAME, dev->vdev.num);
    return 0;

err_v4l2:
    v4l2_device_unregister(&dev->v4l2_dev);
    return ret;
}

static int cap_remove(struct platform_device *pdev)
{
    struct cap_dev *dev = platform_get_drvdata(pdev);

    pm_runtime_disable(&pdev->dev);
    video_unregister_device(&dev->vdev);
    vb2_queue_release(&dev->queue);
    v4l2_device_unregister(&dev->v4l2_dev);

    if (!IS_ERR_OR_NULL(dev->clk))
        clk_disable_unprepare(dev->clk);

    return 0;
}

static const struct of_device_id cap_of_match[] = {
    { .compatible = "vendor,dmabuf-capture-v1" },
    { }
};
MODULE_DEVICE_TABLE(of, cap_of_match);

static struct platform_driver cap_driver = {
    .probe  = cap_probe,
    .remove = cap_remove,
    .driver = {
        .name           = DRV_NAME,
        .of_match_table = cap_of_match,
        .pm             = &pm_runtime_pm_ops,
    },
};

module_platform_driver(cap_driver);

MODULE_AUTHOR("Arun Kumar A S <arunajayakumar96@gmail.com>");
MODULE_DESCRIPTION("V4L2 Video Capture with IOMMU-backed DMABUF zero-copy pipeline");
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL v2");
