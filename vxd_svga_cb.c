/*****************************************************************************

Copyright (c) 2024 Jaroslav Hensl <emulator@emulace.cz>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*****************************************************************************/

/* 32 bit RING-0 code for SVGA-II 3D acceleration */
#define SVGA

#include <limits.h>

#include "winhack.h"
#include "vmm.h"
#include "vxd.h"
#include "vxd_lib.h"

#include "svga_all.h"
#include "3d_accel.h"
#include "code32.h"
#include "vxd_svga.h"
#include "vxd_strings.h"

#include "svga_ver.h"

/*
 * types
 */

#define CBQ_PRESENT 0x01
#define CBQ_RENDER  0x02
#define CBQ_UPDATE  0x04

#pragma pack(push)
#pragma pack(1)
typedef struct _cb_enable_t
{
	uint32             cmd;
	SVGADCCmdStartStop cbstart;
} cb_enable_t;

typedef struct _cb_queue_t
{
	struct _cb_queue_t *next;
	DWORD  flags;
	DWORD  data_size;
	DWORD  pad[13];
} cb_queue_t;

#pragma pack(pop)

typedef struct _cb_queue_info_t
{
	cb_queue_t *first;
	cb_queue_t *last;
} cb_queue_info_t;

/*
 * Globals
 */
extern BOOL gb_support;
extern BOOL cb_support;
extern BOOL cb_context0;

//extern DWORD present_fence;
extern BOOL ST_FB_invalid;

BOOL CB_queue_check(SVGACBHeader *tracked);
inline BOOL CB_queue_check_inline(SVGACBHeader *tracked);

/*
 * Locals
 **/
static cb_queue_info_t cb_queue_info = {NULL, NULL};
static uint64 cb_next_id = {0, 0};

/*
 * Macros
 */
#define WAIT_FOR_CB(_cb, _forcesync) \
	do{ \
		/*dbg_printf(dbg_wait_cb, __LINE__);*/ \
		while(!CB_queue_check_inline(_cb)){ \
			WAIT_FOR_CB_SYNC_ ## _forcesync \
		} \
	}while(0)

/* expansions of WAIT_FOR_CB */
#define WAIT_FOR_CB_SYNC_0
#define WAIT_FOR_CB_SYNC_1 SVGA_Sync();

/* wait for all commands */
void SVGA_Flush_CB_critical()
{
	/* wait for actual CB */
	while(!CB_queue_check(NULL))
	{
		SVGA_Sync();
	}
	
	/* drain FIFO */
	SVGA_Flush();
}

void SVGA_Flush_CB()
{
	Begin_Critical_Section(0);
	SVGA_Flush_CB_critical();
	End_Critical_Section();
}

/**
 * Allocate memory for command buffer
 **/
DWORD *SVGA_CMB_alloc_size(DWORD datasize)
{
	DWORD phy;
	SVGACBHeader *cb;
	cb_queue_t *q;
	
	Begin_Critical_Section(0);
	
	q = (cb_queue_t*)_PageAllocate(RoundToPages(datasize+sizeof(SVGACBHeader)+sizeof(cb_queue_t)), PG_SYS, 0, 0, 0x0, 0x100000, &phy, PAGECONTIG | PAGEUSEALIGN | PAGEFIXED);
	
	if(q)
	{
		q->next = NULL;
		q->flags = 0;
		q->data_size = 0;
		
		cb = (SVGACBHeader*)(q+1);
		
		memset(cb, 0, sizeof(SVGACBHeader));
		cb->status = SVGA_CB_STATUS_COMPLETED; /* important to sync between commands */
		cb->ptr.pa.hi   = 0;
		cb->ptr.pa.low  = phy + sizeof(cb_queue_t) + sizeof(SVGACBHeader);	
		
		End_Critical_Section();
		return (DWORD*)(cb+1);
	}
	End_Critical_Section();
	
	return NULL;
}

void SVGA_CMB_free(DWORD *cmb)
{
	SVGACBHeader *cb = ((SVGACBHeader *)cmb)-1;
	
	Begin_Critical_Section(0);
	
	WAIT_FOR_CB(cb, 1);
		
	_PageFree(cb-1, 0);
	
	End_Critical_Section();
}

DWORD *SVGA_CMB_alloc()
{
	return SVGA_CMB_alloc_size(SVGA_CB_MAX_SIZE);
}

// FIXME: inline?
static void SVGA_cb_id_inc()
{
	_asm
	{
		inc dword ptr [cb_next_id]
		adc dword ptr [cb_next_id+4], 0
	}
}

#ifdef DBGPRINT
#include "vxd_svga_debug.h"
#endif

/*
 * @param tracked: check specific CB, or NULL to check full queue
 *
 * @return: TRUE if tracked is complete or TRUE id queue is empty
 
 */
inline BOOL CB_queue_check_inline(SVGACBHeader *tracked)
{
	cb_queue_t *last = NULL;
	cb_queue_t *item = cb_queue_info.first;
	BOOL in_queue = FALSE;
	BOOL need_restart = FALSE;
	
	while(item != NULL)
	{
		SVGACBHeader *cb = (SVGACBHeader*)(item+1);
		if(cb->status >= SVGA_CB_STATUS_COMPLETED)
		{
			if(last == NULL)
			{
				cb_queue_info.first = item->next;
			}
			else
			{
				last->next = item->next;
			}
			
			if(cb->status > SVGA_CB_STATUS_COMPLETED)
			{
				#ifdef DBGPRINT	
				{
					DWORD *cmd_ptr = (DWORD*)(cb+1);
					
					dbg_printf(dbg_cb_error, cb->status, cb->errorOffset, cmd_ptr[cb->errorOffset/4]);
				}
				#endif
				need_restart = TRUE;
			}
			
			item = item->next;
		}
		else
		{
			if(tracked == cb)
			{
				in_queue = TRUE;
			}
			last = item;
			item = item->next;
		}
	}
	
	cb_queue_info.last = last;
	
	if(need_restart)
	{
		SVGA_CB_restart();
		return TRUE; /* queue is always empty on restart */
	}
	
	if(cb_queue_info.first == NULL)
	{
		return TRUE;
	}
	
	if(tracked != NULL && in_queue == FALSE)
	{
		return TRUE;
	}
	
	return FALSE;	
}

BOOL CB_queue_check(SVGACBHeader *tracked)
{
//	dbg_printf(dbg_queue_check);
	return CB_queue_check_inline(tracked);
}

static BOOL CB_queue_is_flags_set(DWORD flags)
{
	cb_queue_t *item = cb_queue_info.first;
	
	if(flags == 0) return FALSE;
	
	while(item != NULL)
	{
		if((item->flags & flags) != 0)
		{
			return TRUE;
		}
		
		item = item->next;
	}
	
	return FALSE;
}

void CB_queue_insert(SVGACBHeader *cb, DWORD flags)
{
	cb_queue_t *item = (cb_queue_t*)(cb-1);
	item->next = NULL;
	item->flags = flags;
	item->data_size = cb->length;

	if(cb_queue_info.last != NULL)
	{
		cb_queue_info.last->next = item;
		cb_queue_info.last = item;
	}
	else
	{
		cb_queue_info.first = item;
		cb_queue_info.last  = item;
	}
}

void CB_queue_erase()
{
	cb_queue_t *item = cb_queue_info.first;
	while(item != NULL)
	{
		SVGACBHeader *cb = (SVGACBHeader*)(item+1);
		
		cb->status = SVGA_CB_STATUS_QUEUE_FULL;
		
		item = item->next;
	}
	
	cb_queue_info.first = NULL;
	cb_queue_info.last  = NULL;
}

static DWORD flags_to_cbq(DWORD cb_flags)
{
	DWORD r = 0;
	
	if((cb_flags & SVGA_CB_PRESENT) != 0)
	{
		r |= CBQ_PRESENT;
	}
	
	if((cb_flags & SVGA_CB_RENDER) != 0)
	{
		r |= CBQ_RENDER;
	}
	
	if((cb_flags & SVGA_CB_UPDATE) != 0)
	{
		r |= SVGA_CB_UPDATE;
	}
	
	return r;
}

static DWORD flags_to_cbq_check(DWORD cb_flags)
{
	DWORD r = 0;

	if((cb_flags & SVGA_CB_PRESENT) != 0)
	{
		r |= CBQ_PRESENT | CBQ_RENDER;
	}
	
	if((cb_flags & SVGA_CB_RENDER) != 0)
	{
		r |= CBQ_RENDER | SVGA_CB_UPDATE;
	}
	
	if((cb_flags & SVGA_CB_UPDATE) != 0)
	{
		r |= CBQ_RENDER | SVGA_CB_UPDATE;
	}
	
	return r;
}

static uint32 fence_present = 0;
static uint32 fence_render  = 0;
static uint32 fence_update  = 0;

static void flags_fence_check(DWORD cb_flags)
{
	DWORD to_check = flags_to_cbq_check(cb_flags);
	
	if((to_check & CBQ_PRESENT) != 0)
	{
		if(fence_present)
		{
			SVGA_fence_wait(fence_present);
			fence_present = 0;
		}
	}
	
	if((to_check & CBQ_RENDER) != 0)
	{
		if(fence_render)
		{
			SVGA_fence_wait(fence_render);
			fence_render = 0;
		}
	}
	
	if((to_check & CBQ_UPDATE) != 0)
	{
		if(fence_update)
		{
			SVGA_fence_wait(fence_update);
			fence_update = 0;
		}
	}
}

static void flags_fence_insert(DWORD cb_flags, uint32 fence)
{
	if((cb_flags & SVGA_CB_PRESENT) != 0)
	{
		fence_present = fence;
	}
	
	if((cb_flags & SVGA_CB_RENDER) != 0)
	{
		fence_render = fence;
	}
	
	if((cb_flags & SVGA_CB_UPDATE) != 0)
	{
		fence_update = fence;
	}
}

#define flags_fifo_fence_need(_flags) (((_flags) & (SVGA_CB_SYNC | SVGA_CB_FORCE_FENCE | SVGA_CB_PRESENT | SVGA_CB_RENDER | SVGA_CB_UPDATE)) != 0)
#define flags_cb_fence_need(_flags) (((_flags) & (SVGA_CB_FORCE_FENCE)) != 0)

static void SVGA_CMB_submit_critical(DWORD FBPTR cmb, DWORD cmb_size, SVGA_CMB_status_t FBPTR status, DWORD flags, DWORD DXCtxId)
{
	DWORD fence = 0;
	SVGACBHeader *cb = ((SVGACBHeader *)cmb)-1;
	
	if(status)
	{
		cb->status = SVGA_CB_STATUS_NONE;
		status->sStatus = SVGA_PROC_NONE;
		status->qStatus = (volatile DWORD*)&cb->status;
	}

#ifdef DBGPRINT
//	debug_cmdbuf(cmb, cmb_size);
//	debug_cmdbuf_trace(cmb, cmb_size, SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN);
//		debug_draw(cmb, cmb_size);
#endif
	
	if(flags & SVGA_CB_DIRTY_SURFACE)
	{
		ST_FB_invalid = TRUE;
	}

	if(  /* CB are supported and enabled */
		cb_support && cb_context0 &&
		(flags & SVGA_CB_FORCE_FIFO) == 0
	)
	{
		/***
		 *
		 * COMMAND BUFFER procesing
		 *
		 ***/
		DWORD cbhwctxid = SVGA_CB_CONTEXT_0;
		DWORD cbq_check = flags_to_cbq_check(flags);
		
		//if(flags & SVGA_CB_USE_CONTEXT_DEVICE)	cbhwctxid = SVGA_CB_CONTEXT_DEVICE;
		
		if(flags_cb_fence_need(flags))
		{
			DWORD dwords = cmb_size/sizeof(DWORD);
			fence = SVGA_fence_get();
			cmb[dwords]   = SVGA_CMD_FENCE;
			cmb[dwords+1] = fence;
			cmb_size += sizeof(DWORD)*2;
		}
		
		/* wait */
		do
		{
			CB_queue_check_inline(NULL);
		} while(CB_queue_is_flags_set(cbq_check));
		
		if(cmb_size == 0)
		{
			cb->status = SVGA_PROC_COMPLETED;
			if(status)
			{
				status->sStatus = SVGA_PROC_COMPLETED;
				status->qStatus = NULL;
				status->fifo_fence_used = 0;
			}
		}
		else
		{
			cb->status      = SVGA_CB_STATUS_NONE;
			cb->errorOffset = 0;
			cb->offset      = 0; /* VMware modified this, needs to be clear */
			cb->flags       = SVGA_CB_FLAG_NO_IRQ;
			
			if(flags & SVGA_CB_FLAG_DX_CONTEXT)
			{
				cb->flags |= SVGA_CB_FLAG_DX_CONTEXT;
				cb->dxContext = DXCtxId;
			}
			else
			{
				cb->dxContext = 0;
			}
			
			cb->id.low      = cb_next_id.low;
			cb->id.hi       = cb_next_id.hi;
			cb->length      = cmb_size;
			
			CB_queue_insert(cb, flags_to_cbq(flags));			
			
			SVGA_WriteReg(SVGA_REG_COMMAND_HIGH, 0); // high part of 64-bit memory address...
			SVGA_WriteReg(SVGA_REG_COMMAND_LOW, (cb->ptr.pa.low - sizeof(SVGACBHeader)) | cbhwctxid);
			SVGA_Sync(); /* notify HV to read registers (VMware needs it) */
			
			SVGA_cb_id_inc();	
			
			if(flags & SVGA_CB_SYNC)
			{
				WAIT_FOR_CB(cb, 0);
				
				if(cb->status != SVGA_CB_STATUS_COMPLETED)
				{
					dbg_printf(dbg_cmd_error, cb->status, cmb[0], cb->errorOffset);
					if(flags & SVGA_CB_FORCE_FENCE)
					{
						/* this may cause freeze, when fence is in buffer and isn't complete. So return some passed fence  */
						fence = SVGA_fence_passed();
					}
				}
				/*else
				{
					dbg_printf(dbg_cb_suc);
				}*/
				
				if(status)
				{
					status->sStatus = SVGA_PROC_COMPLETED;
					status->qStatus = NULL;
					status->fifo_fence_used = fence;
				}
			}
			else
			{
				if(status)
				{
					status->sStatus = SVGA_PROC_NONE;
					status->qStatus = (volatile DWORD*)&cb->status;
					status->fifo_fence_used = fence;
				}
			}
		}	
	}
	else
	{
		/***
		 *
		 * FIFO procesing
		 *
		 ***/
		DWORD *ptr = cmb;
		DWORD dwords = cmb_size/sizeof(DWORD);
		DWORD nextCmd, max, min;
		
		/* insert fence CMD */
		if(flags_fifo_fence_need(flags))
		{
			fence = SVGA_fence_get();
			ptr[dwords++] = SVGA_CMD_FENCE;
			ptr[dwords++] = fence;
		}
		
		flags_fence_check(flags);
		
		if(dwords == 0)
		{
			cb->status = SVGA_PROC_COMPLETED;
			if(status)
			{
				status->sStatus = SVGA_PROC_COMPLETED;
				status->qStatus = NULL;
				status->fifo_fence_used = 0;
			}
		}
		else
		{
			nextCmd = gSVGA.fifoMem[SVGA_FIFO_NEXT_CMD];
			max     = gSVGA.fifoMem[SVGA_FIFO_MAX];
			min     = gSVGA.fifoMem[SVGA_FIFO_MIN];
			
			/* copy to fifo */
			while(dwords > 0)
			{
				gSVGA.fifoMem[nextCmd/sizeof(DWORD)] = *ptr;
				ptr++;
				
				nextCmd += sizeof(uint32);
				if (nextCmd >= max)
				{
					nextCmd = min;
				}
				gSVGA.fifoMem[SVGA_FIFO_NEXT_CMD] = nextCmd;
				dwords--;
			}
			
			if(flags & SVGA_CB_SYNC)
			{
				SVGA_fence_wait(fence);
				if(status)
				{
					status->sStatus = SVGA_PROC_COMPLETED;
					status->qStatus = NULL;
					status->fifo_fence_used = 0;
				}
			}
			else
			{
				flags_fence_insert(flags, fence);
				
				if(status)
				{
					status->sStatus = SVGA_PROC_FENCE;
					status->qStatus = NULL;
					status->fifo_fence_used = fence;
				}
			}
			
			cb->status = SVGA_PROC_COMPLETED;
		} // size > 0
	} /* FIFO */
	
	if(status)
	{
		status->fifo_fence_last = SVGA_fence_passed();
	}
	
	//dbg_printf(dbg_cmd_off, cmb[0]);
}

void wait_for_cmdbuf()
{
	SVGACBHeader *cb;
	
	Begin_Critical_Section(0);
	cb = ((SVGACBHeader *)cmdbuf)-1;
	WAIT_FOR_CB(cb, 0);
}

void submit_cmdbuf(DWORD cmdsize, DWORD flags, DWORD dx)
{
	SVGA_CMB_submit_critical(cmdbuf, cmdsize, NULL, flags, dx);
	End_Critical_Section();
}

void SVGA_CMB_submit(DWORD FBPTR cmb, DWORD cmb_size, SVGA_CMB_status_t FBPTR status, DWORD flags, DWORD DXCtxId)
{
	Begin_Critical_Section(0);
	SVGA_CMB_submit_critical(cmb, cmb_size, status, flags, DXCtxId);
	End_Critical_Section();
}

static DWORD SVGA_CB_ctr(DWORD data_size)
{
	SVGACBHeader *cb = ((SVGACBHeader *)ctlbuf)-1;
	
	dbg_printf(dbg_ctr_start);

	//Begin_Critical_Section(0);

	cb->status = SVGA_CB_STATUS_NONE;
	cb->errorOffset = 0;
	cb->offset = 0; /* VMware modified this, needs to be clear */
	cb->flags  = SVGA_CB_FLAG_NO_IRQ;
	cb->mustBeZero[0] = 0;
	cb->mustBeZero[1] = 0;
	cb->mustBeZero[2] = 0;
	cb->mustBeZero[3] = 0;
	cb->mustBeZero[4] = 0;
	cb->mustBeZero[5] = 0;
	cb->dxContext = 0;
	cb->id.low = cb_next_id.low;
	cb->id.hi  = cb_next_id.hi;
	cb->length = data_size;

	SVGA_WriteReg(SVGA_REG_COMMAND_HIGH, 0);
	SVGA_WriteReg(SVGA_REG_COMMAND_LOW, (cb->ptr.pa.low - sizeof(SVGACBHeader)) | SVGA_CB_CONTEXT_DEVICE);
	SVGA_Sync();
	
	SVGA_cb_id_inc();

	while(cb->status == SVGA_CB_STATUS_NONE)
	{
		SVGA_Sync();
	}
	
	//End_Critical_Section();
	
	return cb->status;
}

/**
 * GPU10: start context0
 *
 **/
void SVGA_CB_start()
{
	if(cb_support && cb_context0 == FALSE)
	{
		cb_enable_t *cbe = ctlbuf;
		DWORD status;
			
		memset(cbe, 0, sizeof(cb_enable_t));
		cbe->cmd = SVGA_DC_CMD_START_STOP_CONTEXT;
		cbe->cbstart.enable  = 1;
		cbe->cbstart.context = SVGA_CB_CONTEXT_0;
		
		status = SVGA_CB_ctr(sizeof(cb_enable_t));
		
		dbg_printf(dbg_cb_start_status, status);
		
		if(status == SVGA_CB_STATUS_COMPLETED)
		{
			cb_context0 = TRUE;
		}
		else
		{
			cb_support = FALSE;
		}
	}
}

/**
 * GPU10: stop context0
 *
 **/
void SVGA_CB_stop()
{
	cb_context0 = FALSE;
	
	if(cb_support)
	{
		DWORD status;
		cb_enable_t *cbe = ctlbuf;
			
		memset(cbe, 0, sizeof(cb_enable_t));
		cbe->cmd = SVGA_DC_CMD_START_STOP_CONTEXT;
		cbe->cbstart.enable  = 0;
		cbe->cbstart.context = SVGA_CB_CONTEXT_0;
		
		status = SVGA_CB_ctr(sizeof(cb_enable_t));
		
		SVGA_Sync();
		
		CB_queue_erase();
		
		dbg_printf(dbg_cb_stop_status, status);
	}
}

/**
 * GPU10: restart context0 after error
 *
 **/
void SVGA_CB_restart()
{
	SVGA_CB_stop();
	
	SVGA_CB_start();
}

void SVGA_CMB_wait_update()
{
	if(cb_support && cb_context0)
	{
		do
		{
			CB_queue_check_inline(NULL);
		} while(CB_queue_is_flags_set(CBQ_UPDATE | CBQ_UPDATE));
	}
	else
	{
		flags_fence_check(SVGA_CB_UPDATE);
	}
}
