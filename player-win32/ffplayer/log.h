#ifndef __FFPLAYER_LOG_H__
#define __FFPLAYER_LOG_H__

// 包含头文件
#include "stdefine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 函数声明 */
void log_init  (TCHAR *file);
void log_done  (void);
void log_printf(TCHAR *format, ...);

#ifdef __cplusplus
}
#endif

#endif
