/**
 * @file diag_log.c
 * @brief Unified RX diagnostic log helpers.
 */
#include "diag_log.h"
#include "soc_osal.h"

void diag_log_info(const char *message)
{
    osal_printk("[RX_DIAG] %s\r\n", (message != NULL) ? message : "");
}

void diag_log_error(const char *message)
{
    osal_printk("[RX_ERROR] %s\r\n", (message != NULL) ? message : "");
}
