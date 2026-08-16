#ifndef NAOS_FRAMEBUFFER_H
#define NAOS_FRAMEBUFFER_H

#include <linux/fb.h>

/* Source compatibility for existing NaOS clients. New code should include
 * <linux/fb.h> and use the standard framebuffer names directly. */
#define NAOS_FB_IOCTL_GET_VARIABLE_INFO FBIOGET_VSCREENINFO
#define NAOS_FB_IOCTL_GET_FIXED_INFO FBIOGET_FSCREENINFO
#define NAOS_FB_TYPE_PACKED_PIXELS FB_TYPE_PACKED_PIXELS
#define NAOS_FB_VISUAL_TRUECOLOR FB_VISUAL_TRUECOLOR
#define NAOS_FB_ACCEL_NONE FB_ACCEL_NONE

typedef struct fb_bitfield naos_fb_bitfield;
typedef struct fb_fix_screeninfo naos_fb_fixed_info;
typedef struct fb_var_screeninfo naos_fb_variable_info;

#endif
