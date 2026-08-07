/*
 * haas_plugin.c — HAAS-parity user M-codes for the haasSender firmware branch.
 *
 * M97 P<n> [L<m>] — HAAS local subprogram call: jump to the block labelled
 * N<n> in the SAME file, run to M99, return to the block after the M97;
 * L repeats the call (default 1). Semantics per the 2014 Mill Operator's
 * Manual §6.3 (printed p.333).
 *
 * Only meaningful when the board itself is reading the program (SD `$F=`
 * runs and macros): a job streamed line-by-line has no file to seek, so
 * validation fails with error 20 rather than silently continuing — in a
 * trainer a silent no-op teaches a wrong lesson.
 *
 * ponytail: M97 inside a G65/M98 SD macro is refused (validate checks that
 * no other macro layer owns the M99 return); lift by stacking with
 * sdcard/macros.c if anyone ever nests that deep.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include <string.h>

#include "driver.h"

#include "grbl/stream_file.h"
#include "grbl/vfs.h"
#include "grbl/nuts_bolts.h"

#define M97_STACK_DEPTH 8
#define MCode_HaasSubCall ((user_mcode_t)97)

typedef struct {
    vfs_file_t *file;
    size_t return_pos;
    line_number_t return_line;
    size_t target_pos;
    line_number_t target_line;
    uint32_t repeats;
} m97_frame_t;

static m97_frame_t m97_stack[M97_STACK_DEPTH];
static int_fast8_t m97_sp = -1;
static size_t seek_pos;             // target found by validate, used by execute
static line_number_t seek_line;
static uint32_t seek_repeats;

static user_mcode_ptrs_t user_mcode;
static on_report_options_ptr on_report_options;
static on_reset_ptr on_reset;
static on_file_end_ptr on_file_end;

static void m97_return (void);

// Scan the file from the top for a line whose first word is N<target>.
// Sets seek_pos to the start of that line and seek_line to its 1-based line
// number. The caller restores the file position — this leaves it at the scan.
static bool find_nblock (vfs_file_t *file, uint32_t target)
{
    char buf[128];
    size_t n, i, pos = 0, line_start = 0;
    line_number_t line = 1;
    enum { LineStart, InN, Skip } state = LineStart;
    uint32_t nval = 0;
    bool has_digits = false;

    vfs_seek(file, 0);

    while((n = vfs_read(buf, 1, sizeof(buf), file)) > 0) {
        for(i = 0; i < n; i++, pos++) {
            char c = buf[i];
            if(c == '\n') {
                state = LineStart;
                line++;
                line_start = pos + 1;
                continue;
            }
            switch(state) {
                case LineStart:
                    if(c == ' ' || c == '\t' || c == '\r')
                        break;
                    if(c == 'N' || c == 'n') {
                        state = InN;
                        nval = 0;
                        has_digits = false;
                    } else
                        state = Skip;
                    break;
                case InN:
                    if(c >= '0' && c <= '9') {
                        nval = nval * 10 + (uint32_t)(c - '0');
                        has_digits = true;
                    } else {
                        if(has_digits && nval == target) {
                            seek_pos = line_start;
                            seek_line = line;
                            return true;
                        }
                        state = Skip;
                    }
                    break;
                case Skip:
                    break;
            }
        }
    }

    // N<target> as the last token of a file with no trailing newline
    if(state == InN && has_digits && nval == target) {
        seek_pos = line_start;
        seek_line = line;
        return true;
    }

    return false;
}

static user_mcode_type_t mcode_check (user_mcode_t mcode)
{
    return mcode == MCode_HaasSubCall
            ? UserMCode_Normal
            : (user_mcode.check ? user_mcode.check(mcode) : UserMCode_Unsupported);
}

static status_code_t mcode_validate (parser_block_t *gc_block)
{
    if(gc_block->user_mcode != MCode_HaasSubCall)
        return user_mcode.validate ? user_mcode.validate(gc_block) : Status_Unhandled;

    if(!gc_block->words.p || !isintf(gc_block->values.p) || gc_block->values.p < 1.0f)
        return Status_GcodeValueOutOfRange;

    if(gc_block->words.l && (!isintf(gc_block->values.l) || gc_block->values.l < 1.0f))
        return Status_GcodeValueOutOfRange;

    if(hal.stream.file == NULL)                 // streamed job: nothing to seek
        return Status_GcodeUnsupportedCommand;

    if(grbl.on_macro_return && grbl.on_macro_return != m97_return)
        return Status_GcodeUnsupportedCommand;  // an SD macro owns the M99 return

    if(m97_sp >= M97_STACK_DEPTH - 1)
        return Status_FlowControlStackOverflow;

    size_t restore = vfs_tell(hal.stream.file);
    bool found = find_nblock(hal.stream.file, (uint32_t)gc_block->values.p);
    vfs_seek(hal.stream.file, restore);

    if(!found)
        return Status_GcodeValueOutOfRange;     // no such N block in this file

    seek_repeats = gc_block->words.l ? (uint32_t)gc_block->values.l : 1;
    gc_block->words.p = gc_block->words.l = Off;

    return Status_OK;
}

static void mcode_execute (sys_state_t state, parser_block_t *gc_block)
{
    if(gc_block->user_mcode != MCode_HaasSubCall) {
        if(user_mcode.execute)
            user_mcode.execute(state, gc_block);
        return;
    }

    m97_frame_t *frame = &m97_stack[++m97_sp];

    frame->file = hal.stream.file;
    frame->return_pos = vfs_tell(hal.stream.file);
    frame->return_line = (line_number_t)gc_block->values.n;
    frame->target_pos = seek_pos;
    frame->target_line = seek_line;
    frame->repeats = seek_repeats;

    if(grbl.on_macro_return == NULL)
        grbl.on_macro_return = m97_return;

    stream_reposition(frame->file, frame->target_pos, frame->target_line);
}

// M99 while an M97 frame is active: repeat the sub or return past the call.
static void m97_return (void)
{
    if(m97_sp < 0)
        return;

    m97_frame_t *frame = &m97_stack[m97_sp];

    if(--frame->repeats > 0)
        stream_reposition(frame->file, frame->target_pos, frame->target_line);
    else {
        stream_reposition(frame->file, frame->return_pos, frame->return_line);
        if(--m97_sp < 0 && grbl.on_macro_return == m97_return)
            grbl.on_macro_return = NULL;
    }
}

static void onReset (void)
{
    m97_sp = -1;
    if(grbl.on_macro_return == m97_return)
        grbl.on_macro_return = NULL;

    if(on_reset)
        on_reset();
}

// A file that ends with frames still active had an M97 with no M99 —
// drop the stale frames so they cannot hijack a later job's M99.
static status_code_t onFileEnd (vfs_file_t *handle, status_code_t status)
{
    while(m97_sp >= 0 && m97_stack[m97_sp].file == handle)
        m97_sp--;

    if(m97_sp < 0 && grbl.on_macro_return == m97_return)
        grbl.on_macro_return = NULL;

    return on_file_end ? on_file_end(handle, status) : status;
}

static void onReportOptions (bool newopt)
{
    on_report_options(newopt);

    if(!newopt)
        report_plugin("HAAS parity", "0.1");
}

void my_plugin_init (void)
{
    memcpy(&user_mcode, &grbl.user_mcode, sizeof(user_mcode_ptrs_t));
    grbl.user_mcode.check = mcode_check;
    grbl.user_mcode.validate = mcode_validate;
    grbl.user_mcode.execute = mcode_execute;

    on_report_options = grbl.on_report_options;
    grbl.on_report_options = onReportOptions;

    on_reset = grbl.on_reset;
    grbl.on_reset = onReset;

    on_file_end = grbl.on_file_end;
    grbl.on_file_end = onFileEnd;
}
