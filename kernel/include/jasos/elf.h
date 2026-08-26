#pragma once

#include <jasos/types.h>
#include <jasos/status.h>
#include <jasos/ke.h>

status_t elf_load(process_t *p, const u8 *image, u64 len, virt_t *entry_out);
