

#ifndef __CNAS_HSM_PRE_PROC_TBL_H__
#define __CNAS_HSM_PRE_PROC_TBL_H__

/*****************************************************************************
  1 The Include of the header file
*****************************************************************************/
#include "vos.h"
#include "NasFsm.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif


#pragma pack(4)

/*****************************************************************************
  2 The Macro Define
*****************************************************************************/
extern NAS_STA_STRU                                         g_astCnasHsmPreProcStaTbl[];

#define CNAS_HSM_GetPreProcStaTbl()                             (g_astCnasHsmPreProcStaTbl)

/*****************************************************************************
  3 The Enumeration Define
*****************************************************************************/



/*****************************************************************************
  4 The Declaration Of The Gloabal Variable
*****************************************************************************/

/*****************************************************************************
  5 The Define Of the Message Header
*****************************************************************************/


/*****************************************************************************
  6 The Define of the Message Name
*****************************************************************************/


/*****************************************************************************
  7 The Struct Define
*****************************************************************************/




/*****************************************************************************
  8 The Union Define
*****************************************************************************/


/*****************************************************************************
  9 Other Defines
*****************************************************************************/


/*****************************************************************************
  10 The Declaration Of The Function
*****************************************************************************/
#if (FEATURE_ON == FEATURE_UE_MODE_CDMA)
extern VOS_VOID  CNAS_HSM_RegisterPreFsm(VOS_VOID);
extern VOS_UINT32  CNAS_HSM_GetPreProcStaTblSize(VOS_VOID);
extern NAS_FSM_DESC_STRU* CNAS_HSM_GetPreProcFsmDescAddr(VOS_VOID);

#endif


#if (VOS_OS_VER == VOS_WIN32)
#pragma pack()
#else
#pragma pack(0)
#endif




#ifdef __cplusplus
    #if __cplusplus
        }
    #endif
#endif

#endif /* end of CnasHsmFsmTbl.h */


