#ifndef _SVGA_VER_H_
#define _SVGA_VER_H_

#define VMWGFX_DRIVER_DATE "20211206"
#define VMWGFX_DRIVER_MAJOR 2
#define VMWGFX_DRIVER_MINOR 20
#define VMWGFX_DRIVER_PATCHLEVEL 0

#define LINUX_VERSION_MAJOR      6
#define LINUX_VERSION_PATCHLEVEL 9
#define LINUX_VERSION_SUBLEVEL   10

/* constants from linux driver */
#define VMWGFX_MAX_DISPLAYS 16

#define VMWGFX_NUM_GB_CONTEXT 256
#define VMWGFX_NUM_GB_SHADER 20000
#define VMWGFX_NUM_GB_SURFACE 32768
#define VMWGFX_NUM_GB_SCREEN_TARGET VMWGFX_MAX_DISPLAYS
#define VMWGFX_NUM_DXCONTEXT 256
#define VMWGFX_NUM_DXQUERY 512
#define VMWGFX_NUM_MOB (VMWGFX_NUM_GB_CONTEXT +\
			VMWGFX_NUM_GB_SHADER +\
			VMWGFX_NUM_GB_SURFACE +\
			VMWGFX_NUM_GB_SCREEN_TARGET)

#endif
