// This library is part of PLINK 2.00, copyright (C) 2005-2019 Shaun Purcell,
// Christopher Chang.
//
// This library is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This library is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this library.  If not, see <http://www.gnu.org/licenses/>.

#include "plink2_decompress.h"

#ifdef __cplusplus
namespace plink2 {
#endif

PglErr gzopen_read_checked(const char* fname, gzFile* gzf_ptr) {
  *gzf_ptr = gzopen(fname, FOPEN_RB);
  if (unlikely(!(*gzf_ptr))) {
    logputs("\n");
    logerrprintfww(kErrprintfFopen, fname);
    return kPglRetOpenFail;
  }
  if (unlikely(gzbuffer(*gzf_ptr, 131072))) {
    return kPglRetNomem;
  }
  return kPglRetSuccess;
}

PglErr GzopenAndSkipFirstLines(const char* fname, uint32_t lines_to_skip, uintptr_t loadbuf_size, char* loadbuf, gzFile* gzf_ptr) {
  PglErr reterr = gzopen_read_checked(fname, gzf_ptr);
  if (unlikely(reterr)) {
    return reterr;
  }
  loadbuf[loadbuf_size - 1] = ' ';
  for (uint32_t line_idx = 1; line_idx <= lines_to_skip; ++line_idx) {
    if (unlikely(!gzgets(*gzf_ptr, loadbuf, loadbuf_size))) {
      if (gzeof(*gzf_ptr)) {
        logerrprintfww("Error: Fewer lines than expected in %s.\n", fname);
        return kPglRetInconsistentInput;
      }
      return kPglRetReadFail;
    }
    if (unlikely(!loadbuf[loadbuf_size - 1])) {
      if ((loadbuf_size == kMaxMediumLine) || (loadbuf_size == kMaxLongLine)) {
        logerrprintfww("Error: Line %u of %s is pathologically long.\n", line_idx, fname);
        return kPglRetMalformedInput;
      }
      return kPglRetNomem;
    }
  }
  return kPglRetSuccess;
}


void PreinitGzTokenStream(GzTokenStream* gtsp) {
  gtsp->gz_infile = nullptr;
}

PglErr InitGzTokenStream(const char* fname, GzTokenStream* gtsp, char* buf_start) {
  PglErr reterr = gzopen_read_checked(fname, &(gtsp->gz_infile));
  if (unlikely(reterr)) {
    return reterr;
  }
  gtsp->buf_start = buf_start;
  gtsp->read_iter = &(buf_start[kMaxMediumLine]);
  gtsp->buf_end = gtsp->read_iter;
  gtsp->buf_end[0] = '0';  // force initial load
  return kPglRetSuccess;
}

char* AdvanceGzTokenStream(GzTokenStream* gtsp, uint32_t* token_slen_ptr) {
  char* token_start = gtsp->read_iter;
  char* buf_end = gtsp->buf_end;
 AdvanceGzTokenStream_restart:
  while (ctou32(*token_start) <= ' ') {
    ++token_start;
  }
  while (1) {
    if (token_start < buf_end) {
      char* token_end = &(token_start[1]);
      while (ctou32(*token_end) > ' ') {
        ++token_end;
      }
      const uint32_t token_slen = token_end - token_start;
      if (token_end < buf_end) {
        *token_slen_ptr = token_slen;
        gtsp->read_iter = &(token_end[1]);
        return token_start;
      }
      if (unlikely(token_slen > kMaxMediumLine)) {
        *token_slen_ptr = UINT32_MAX;
        return nullptr;
      }
      char* new_token_start = &(gtsp->buf_start[kMaxMediumLine - token_slen]);
      memcpy(new_token_start, token_start, token_slen);
      token_start = new_token_start;
    } else {
      token_start = &(gtsp->buf_start[kMaxMediumLine]);
    }
    char* load_start = &(gtsp->buf_start[kMaxMediumLine]);
    const int32_t bufsize = gzread(gtsp->gz_infile, load_start, kMaxMediumLine);
    if (unlikely(bufsize < 0)) {
      *token_slen_ptr = UINT32_MAXM1;
      return nullptr;
    }
    buf_end = &(load_start[S_CAST(uint32_t, bufsize)]);
    buf_end[0] = ' ';
    buf_end[1] = '0';
    gtsp->buf_end = buf_end;
    if (!bufsize) {
      if (unlikely(!gzeof(gtsp->gz_infile))) {
        *token_slen_ptr = UINT32_MAXM1;
        return nullptr;
      }
      // bufsize == 0, eof
      if (token_start == load_start) {
        *token_slen_ptr = 0;
        return nullptr;
      }
      gtsp->read_iter = load_start;
      *token_slen_ptr = load_start - token_start;
      return token_start;
    }
    if (token_start == load_start) {
      goto AdvanceGzTokenStream_restart;
    }
  }
}

BoolErr CloseGzTokenStream(GzTokenStream* gtsp) {
  if (!gtsp->gz_infile) {
    return 0;
  }
  return gzclose_null(&(gtsp->gz_infile));
}


void PreinitRLstream(ReadLineStream* rlsp) {
  rlsp->gz_infile = nullptr;
  rlsp->bgz_infile = nullptr;
  rlsp->bgzf_decompress_thread_ct = 1;
#ifdef _WIN32
  // bugfix (7 Mar 2018): forgot to initialize this
  rlsp->read_thread = nullptr;
#else
  rlsp->sync_init_state = 0;
#endif
}

PglErr IsBgzf(const char* fname, uint32_t* is_bgzf_ptr) {
  // This takes the place of htslib check_header().
  FILE* infile = fopen(fname, FOPEN_RB);
  if (unlikely(!infile)) {
    // Note that this does not print an error message (since it may be called
    // by a worker thread).
    return kPglRetOpenFail;
  }
  unsigned char buf[16];
  if (unlikely(!fread_unlocked(buf, 16, 1, infile))) {
    fclose(infile);
    return kPglRetReadFail;
  }
  *is_bgzf_ptr = memequal_k(buf, "\37\213\10", 3) && ((buf[3] & 4) != 0) && memequal_k(&(buf[10]), "\6\0BC\2", 6);
  fclose(infile);
  return kPglRetSuccess;
}

// This type of code is especially bug-prone (ESR would call it a "defect
// attractor").  Goal is to get it right, and fast enough to be a major win
// over gzgets()... and then not worry about it again for years.
THREAD_FUNC_DECL ReadLineStreamThread(void* arg) {
  ReadLineStream* context = R_CAST(ReadLineStream*, arg);
  ReadLineStreamSync* syncp = context->syncp;
  gzFile gz_infile = context->gz_infile;
  BGZF* bgz_infile = context->bgz_infile;
  char* buf = context->buf;
  char* buf_end = context->buf_end;
  char* cur_block_start = buf;
  char* read_head = buf;

  // We can either be reading/decompressing into memory past the bytes passed
  // to the consumer, or we can be doing it before those bytes.
  // In the first case, read_stop is buf_end, but it gets changed to the
  // latest value of consume_tail when we return to the front of the buffer.
  // In the second case, read_stop is the position of the first passed byte.
  // todo: change how buf_end works so that long lines are consistently
  // detected.
  char* read_stop = buf_end;
#ifdef _WIN32
  CRITICAL_SECTION* critical_sectionp = &syncp->critical_section;
  HANDLE reader_progress_event = context->reader_progress_event;
  HANDLE consumer_progress_event = context->consumer_progress_event;
#else
  pthread_mutex_t* sync_mutexp = &syncp->sync_mutex;
  pthread_cond_t* reader_progress_condvarp = &syncp->reader_progress_condvar;
  pthread_cond_t* consumer_progress_condvarp = &syncp->consumer_progress_condvar;
#endif
  const uint32_t enforced_max_line_blen = context->enforced_max_line_blen;
  const char* new_fname = nullptr;
  while (1) {
    RlsInterrupt interrupt = kRlsInterruptNone;
    PglErr reterr;
    RlsInterrupt min_interrupt;
    while (1) {
      uintptr_t read_attempt_size = read_stop - read_head;
      if (!read_attempt_size) {
        const uint32_t memmove_required = (read_stop == buf_end);
        if (unlikely((cur_block_start == buf) && memmove_required)) {
          goto ReadLineStreamThread_LONG_LINE;
        }
        // We cannot continue reading forward.  Cases:
        // 1. read_stop == buf_end, cur_block_start != buf.  This means we're
        //    in the middle of reading/decompressing a long line, and want to
        //    wait for consume_tail == cur_block_start, so we can memmove all
        //    the bytes back and continue reading forward.  (Tried
        //    relaxing this to
        //      consume_tail >= (buf_end - cur_block_start) + margin
        //    for various values of margin, but that didn't make a meaningful
        //    difference.)
        // 2. read_stop == buf_end, cur_block_start == buf.  We failed with a
        //    long-line error here.
        // 3. read_stop < buf_end (usual case).  This means the consumer may
        //    not be done handling some bytes-in-front we handed off earlier.
        //    We are waiting for consume_tail <= cur_block_start, which means
        //    all bytes in front have been consumed and we're free to continue
        //    reading forward.
        char* latest_consume_tail;
#ifdef _WIN32
        // bugfix (7 May 2018): when consumer thread is waiting with
        // syncp->consume_tail == cur_block_start, read_stop is near but not at
        // buf_end, and there's no '\n' in the subsequent read, we can reach
        // here a second time without releasing the consumer, so we'd enter
        // deadlock if we unconditionally wait on consumer_progress_event (and
        // in the Linux/OS X case, we'd be waiting for a spurious wakeup to
        // save us).
        // However, if memmove_required isn't true, we have to wait first; see
        // the 21 Mar bugfix.
        if (!memmove_required) {
          goto ReadLineStreamThread_wait_first;
        }
        while (1) {
          EnterCriticalSection(critical_sectionp);
          interrupt = syncp->interrupt;
          if (interrupt != kRlsInterruptNone) {
            goto ReadLineStreamThread_INTERRUPT;
          }
          latest_consume_tail = syncp->consume_tail;
          if (memmove_required) {
            if (latest_consume_tail == cur_block_start) {
              syncp->consume_tail = buf;
              syncp->available_end = buf;
              break;
            }
          } else if (latest_consume_tail <= cur_block_start) {
            // See the 20 Mar bugfix in AdvanceRLstream().
            break;
          }
          LeaveCriticalSection(critical_sectionp);
        ReadLineStreamThread_wait_first:
          WaitForSingleObject(consumer_progress_event, INFINITE);
        }
        // bugfix (23 Mar 2018): didn't always leave the critical section
        LeaveCriticalSection(critical_sectionp);
#else
        pthread_mutex_lock(sync_mutexp);
        if (!memmove_required) {
          // Wait for all bytes in front of read_stop to be consumed.
          goto ReadLineStreamThread_wait_first;
        }
        while (1) {
          interrupt = syncp->interrupt;
          if (interrupt != kRlsInterruptNone) {
            goto ReadLineStreamThread_INTERRUPT;
          }
          latest_consume_tail = syncp->consume_tail;
          if (memmove_required) {
            if (latest_consume_tail == cur_block_start) {
              // All bytes have been consumed; memmove is now safe.
              // bugfix (2 Oct 2018): Previously, this just set
              // syncp->cur_circular_end = cur_block_start, but that created
              // TWO consume_iter == available_end == cur_circular_end cases,
              // one of which was handled incorrectly.
              syncp->consume_tail = buf;
              syncp->available_end = buf;
              break;
            }
            // There are bytes behind cur_block_start that haven't been
            // consumed yet.  This is possible on the first iteration through
            // the loop, since consumer_progress_state may have been set for a
            // reason we aren't interested in.

          } else if (latest_consume_tail <= cur_block_start) {
            // All bytes in front of read_stop have been consumed.
            break;
          }
        ReadLineStreamThread_wait_first:
          while (!syncp->consumer_progress_state) {
            pthread_cond_wait(consumer_progress_condvarp, sync_mutexp);
          }
          syncp->consumer_progress_state = 0;
        }
        pthread_mutex_unlock(sync_mutexp);
#endif
        if (read_stop == buf_end) {
          const uint32_t cur_memmove_len = buf_end - cur_block_start;
          memmove(buf, cur_block_start, cur_memmove_len);
          cur_block_start = buf;
          read_head = &(buf[cur_memmove_len]);
        } else {
          read_stop = buf_end;
        }
        continue;
      }
      if (read_attempt_size > kDecompressChunkSize) {
        read_attempt_size = kDecompressChunkSize;
      }
      int32_t bytes_read;
      // printf("reading %lx..%lx\n", (uintptr_t)read_head, (uintptr_t)(read_head + read_attempt_size));
      if (bgz_infile) {
        bytes_read = bgzf_read(bgz_infile, read_head, read_attempt_size);
        if (unlikely(bytes_read == -1)) {
          goto ReadLineStreamThread_READ_FAIL;
        }
      } else {
        bytes_read = gzread(gz_infile, read_head, read_attempt_size);
      }
      char* cur_read_end = &(read_head[S_CAST(uint32_t, bytes_read)]);
      if (bytes_read < S_CAST(int32_t, S_CAST(uint32_t, read_attempt_size))) {
        if (!bgz_infile) {
          if (unlikely(!gzeof(gz_infile))) {
            goto ReadLineStreamThread_READ_FAIL;
          }
        }
        read_head = cur_read_end;
        if (cur_block_start != read_head) {
          if (read_head[-1] != '\n') {
            // Append '\n' so consumer can always use rawmemchr(., '\n') to
            // find the end of the current line.
            *read_head++ = '\n';
          }
        }
        goto ReadLineStreamThread_EOF;
      }
      char* last_lf = Memrchr(read_head, '\n', read_attempt_size);
      if (last_lf) {
        if (S_CAST(uintptr_t, last_lf - cur_block_start) >= enforced_max_line_blen) {
          if (unlikely(!memchr(read_head, '\n', &(cur_block_start[enforced_max_line_blen]) - read_head))) {
            goto ReadLineStreamThread_LONG_LINE;
          }
        }
        char* next_available_end = &(last_lf[1]);
#ifdef _WIN32
        EnterCriticalSection(critical_sectionp);
#else
        pthread_mutex_lock(sync_mutexp);
#endif
        interrupt = syncp->interrupt;
        if (interrupt != kRlsInterruptNone) {
          goto ReadLineStreamThread_INTERRUPT;
        }
        char* latest_consume_tail = syncp->consume_tail;
        const uint32_t all_later_bytes_consumed = (latest_consume_tail <= cur_block_start);
        const uint32_t return_to_start = all_later_bytes_consumed && (latest_consume_tail >= &(buf[kDecompressChunkSize]));
        if (return_to_start) {
          // bugfix (2 Oct 2018): This was previously setting
          // syncp->available_end = next_available_end too, and that was being
          // handled as a special case which conflicted with a rare legitimate
          // case.
          syncp->cur_circular_end = next_available_end;
          syncp->available_end = buf;
        } else {
          syncp->available_end = next_available_end;
        }
#ifdef _WIN32
        // bugfix (23 Mar 2018): this needs to be in the critical section,
        // otherwise there's a danger of this resetting legitimate progress
        ResetEvent(consumer_progress_event);
        SetEvent(reader_progress_event);
        LeaveCriticalSection(critical_sectionp);
#else
        // bugfix (21 Mar 2018): must force consumer_progress_state to 0 (or
        // ResetEvent(consumer_progress_event); otherwise the other wait loop's
        // read_stop = buf_end assignment may occur before all later bytes are
        // actually consumed, in the next_available_end == latest_consume_tail
        // edge case.
        syncp->consumer_progress_state = 0;
        pthread_cond_signal(reader_progress_condvarp);
        pthread_mutex_unlock(sync_mutexp);
#endif
        if (return_to_start) {
          // Best to return to the beginning of the buffer.
          // (Note that read_attempt_size is guaranteed to be
          // <= kDecompressChunkSize.)
          const uintptr_t trailing_byte_ct = cur_read_end - next_available_end;
          /*
          printf("trailing_byte_ct: %lu\n", (uintptr_t)trailing_byte_ct);
          printf("buf: %lx\n", (uintptr_t)buf);
          printf("latest_consume_tail: %lx\n", (uintptr_t)latest_consume_tail);
          */
          memcpy(buf, next_available_end, trailing_byte_ct);
          cur_block_start = buf;
          read_head = &(buf[trailing_byte_ct]);
          // May as well reduce false sharing risk.
          read_stop = R_CAST(char*, RoundDownPow2(R_CAST(uintptr_t, latest_consume_tail), kCacheline));
          continue;
        }
        if (all_later_bytes_consumed) {
          read_stop = buf_end;
        } else {
          read_stop = R_CAST(char*, RoundDownPow2(R_CAST(uintptr_t, latest_consume_tail), kCacheline));
        }
        cur_block_start = next_available_end;
      }
      read_head = cur_read_end;
    }
    while (0) {
    ReadLineStreamThread_OPEN_FAIL:
      min_interrupt = kRlsInterruptShutdown;
      reterr = kPglRetOpenFail;
      break;
    ReadLineStreamThread_READ_FAIL:
      min_interrupt = kRlsInterruptShutdown;
      reterr = kPglRetReadFail;
      break;
    ReadLineStreamThread_NOMEM:
      min_interrupt = kRlsInterruptShutdown;
      reterr = kPglRetNomem;
      break;
    ReadLineStreamThread_LONG_LINE:
      min_interrupt = kRlsInterruptShutdown;
      reterr = kPglRetLongLine;
      break;
    ReadLineStreamThread_EOF:
      min_interrupt = kRlsInterruptRetarget;
      reterr = kPglRetEof;
      break;
    ReadLineStreamThread_OPEN_OR_READ_FAIL:
      min_interrupt = kRlsInterruptShutdown;
      break;
    }
    // We need to wait for a message from the consumer before we can usefully
    // proceed.
    // More precisely:
    // * In the eof subcase, we're waiting for either a rewind or shutdown
    //   request.
    // * In the error subcase, we're just waiting for the shutdown request.

    // Pass the error code back.
#ifdef _WIN32
    EnterCriticalSection(critical_sectionp);
#else
    pthread_mutex_lock(sync_mutexp);
#endif
    syncp->reterr = reterr;
    interrupt = syncp->interrupt;
    if (interrupt >= min_interrupt) {
      // It's our lucky day: we don't need to wait again.
      goto ReadLineStreamThread_INTERRUPT;
    }
    if (reterr == kPglRetEof) {
      syncp->available_end = read_head;
    }
#ifdef _WIN32
    SetEvent(reader_progress_event);
    LeaveCriticalSection(critical_sectionp);
    while (1) {
      WaitForSingleObject(consumer_progress_event, INFINITE);
      EnterCriticalSection(critical_sectionp);
      interrupt = syncp->interrupt;
      if (interrupt >= min_interrupt) {
        break;
      }
      LeaveCriticalSection(critical_sectionp);
    }
#else
    pthread_cond_signal(reader_progress_condvarp);
    do {
      while (!syncp->consumer_progress_state) {
        pthread_cond_wait(consumer_progress_condvarp, sync_mutexp);
      }
      syncp->consumer_progress_state = 0;
      interrupt = syncp->interrupt;
    } while (interrupt < min_interrupt);
#endif
  ReadLineStreamThread_INTERRUPT:
    // must be in critical section here, or be holding the mutex.
    if (interrupt == kRlsInterruptRetarget) {
      new_fname = syncp->new_fname;
      syncp->interrupt = kRlsInterruptNone;
      syncp->reterr = kPglRetSuccess;
    }
#ifdef _WIN32
    LeaveCriticalSection(critical_sectionp);
#else
    pthread_mutex_unlock(sync_mutexp);
#endif
    if (interrupt == kRlsInterruptShutdown) {
      // possible todo: close the file here
      THREAD_RETURN;
    }
    assert(interrupt == kRlsInterruptRetarget);
    if (!new_fname) {
      if (bgz_infile) {
        if (unlikely(bgzf_seek(bgz_infile, 0, SEEK_SET))) {
          goto ReadLineStreamThread_READ_FAIL;
        }
      } else {
        if (unlikely(gzrewind(gz_infile))) {
          goto ReadLineStreamThread_READ_FAIL;
        }
      }
    } else {
      if (bgz_infile) {
        bgzf_close(bgz_infile);
        bgz_infile = nullptr;
        context->bgz_infile = nullptr;
      } else {
        // don't really care about return value here.
        gzclose(gz_infile);
        gz_infile = nullptr;
        context->gz_infile = nullptr;
      }

      uint32_t new_file_is_bgzf;
      reterr = IsBgzf(new_fname, &new_file_is_bgzf);
      if (unlikely(reterr)) {
        goto ReadLineStreamThread_OPEN_OR_READ_FAIL;
      }
      if (new_file_is_bgzf) {
        bgz_infile = bgzf_open(new_fname, "r");
        context->bgz_infile = bgz_infile;
        if (unlikely(!bgz_infile)) {
          goto ReadLineStreamThread_OPEN_FAIL;
        }
#ifndef _WIN32
        if (context->bgzf_decompress_thread_ct > 1) {
          if (unlikely(bgzf_mt(bgz_infile, context->bgzf_decompress_thread_ct, 128))) {
            goto ReadLineStreamThread_NOMEM;
          }
        }
#endif
      } else {
        // don't use gzopen_read_checked(), since in the error case, both
        // threads may write to g_logbuf simultaneously.
        gz_infile = gzopen(new_fname, FOPEN_RB);
        context->gz_infile = gz_infile;
        if (unlikely(!gz_infile)) {
          goto ReadLineStreamThread_OPEN_FAIL;
        }
        if (unlikely(gzbuffer(gz_infile, 131072))) {
          // is this actually possible?
          goto ReadLineStreamThread_NOMEM;
        }
      }
    }
    cur_block_start = buf;
    read_head = buf;
    read_stop = buf_end;
  }
}

// Probable todos:
// - Make at least 64-bit Windows builds have working multithreaded bgzf
//   compression/decompression.
// - Handle seekable-zstd read/write in essentially the same way as bgzf.
//   Modify compressor stream to default to writing seekable zstd files, with
//   block size on the order of 1-16 MiB.
// - Check this whenever we open a read stream, since even when we only want to
//   use one decompressor thread, we still benefit from knowing a file is bgzf
//   because we can safely use libdeflate instead of zlib under the hood.
PglErr RlsOpenMaybeBgzf(const char* fname, __maybe_unused uint32_t calc_thread_ct, ReadLineStream* rlsp) {
  uint32_t file_is_bgzf;
  PglErr reterr = IsBgzf(fname, &file_is_bgzf);
  if (unlikely(reterr)) {
    return reterr;
  }
  if (file_is_bgzf) {
    rlsp->bgz_infile = bgzf_open(fname, "r");
    if (unlikely(!rlsp->bgz_infile)) {
      return kPglRetOpenFail;
    }
#ifndef _WIN32
    if (calc_thread_ct > 1) {
      // note that the third parameter is now irrelevant
      if (unlikely(bgzf_mt(rlsp->bgz_infile, calc_thread_ct, 128))) {
        return kPglRetNomem;
      }
    }
#else
    calc_thread_ct = 1;
#endif
    rlsp->bgzf_decompress_thread_ct = calc_thread_ct;
  } else {
    rlsp->gz_infile = gzopen(fname, FOPEN_RB);
    if (unlikely(!rlsp->gz_infile)) {
      return kPglRetOpenFail;
    }
    if (unlikely(gzbuffer(rlsp->gz_infile, 131072))) {
      return kPglRetNomem;
    }
  }
  return kPglRetSuccess;
}

PglErr InitRLstreamEx(uint32_t alloc_at_end, uint32_t enforced_max_line_blen, uint32_t max_line_blen, ReadLineStream* rlsp, char** consume_iterp) {
  PglErr reterr = kPglRetSuccess;
  {
    // To avoid a first-line special case, we start consume_iter at position
    // -1, pointing to a \n.  We make this safe by adding 1 to the previous
    // bigstack allocation size.  To simplify rewind/retarget, we also don't
    // let the reader thread overwrite this byte.
    const uintptr_t sync_byte_ct = RoundUpPow2(sizeof(ReadLineStreamSync) + 1, kCacheline);
    const uintptr_t buf_byte_ct = RoundUpPow2(max_line_blen + kDecompressChunkSize, kCacheline);
    const uintptr_t tot_byte_ct = sync_byte_ct + buf_byte_ct;
    if (unlikely(RoundDownPow2(bigstack_left(), kCacheline) < tot_byte_ct)) {
      goto InitRLstreamEx_ret_NOMEM;
    }
    ReadLineStreamSync* syncp;
    if (!alloc_at_end) {
      syncp = S_CAST(ReadLineStreamSync*, bigstack_alloc_raw(tot_byte_ct));
    } else {
      g_bigstack_end = R_CAST(unsigned char*, RoundDownPow2(R_CAST(uintptr_t, g_bigstack_end), kCacheline) - tot_byte_ct);
      syncp = R_CAST(ReadLineStreamSync*, g_bigstack_end);
    }
    char* buf = R_CAST(char*, R_CAST(uintptr_t, syncp) + sync_byte_ct);
    buf[-1] = '\n';
    rlsp->consume_stop = buf;
    rlsp->syncp = syncp;
    rlsp->buf = buf;
    rlsp->buf_end = &(buf[buf_byte_ct]);
    rlsp->enforced_max_line_blen = enforced_max_line_blen;
    syncp->consume_tail = buf;
    syncp->cur_circular_end = nullptr;
    syncp->available_end = buf;
    syncp->reterr = kPglRetSuccess;
    syncp->interrupt = kRlsInterruptNone;
    syncp->new_fname = nullptr;
#ifdef _WIN32
    // apparently this can raise a low-memory exception in older Windows
    // versions, but that's not really our problem.
    InitializeCriticalSection(&syncp->critical_section);

    rlsp->reader_progress_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (unlikely(!rlsp->reader_progress_event)) {
      DeleteCriticalSection(&syncp->critical_section);
      goto InitRLstreamEx_ret_THREAD_CREATE_FAIL;
    }
    rlsp->consumer_progress_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (unlikely(!rlsp->consumer_progress_event)) {
      DeleteCriticalSection(&syncp->critical_section);
      CloseHandle(rlsp->reader_progress_event);
      goto InitRLstreamEx_ret_THREAD_CREATE_FAIL;
    }
    rlsp->read_thread = R_CAST(HANDLE, _beginthreadex(nullptr, kDefaultThreadStack, ReadLineStreamThread, rlsp, 0, nullptr));
    if (unlikely(!rlsp->read_thread)) {
      DeleteCriticalSection(&syncp->critical_section);
      CloseHandle(rlsp->consumer_progress_event);
      CloseHandle(rlsp->reader_progress_event);
      goto InitRLstreamEx_ret_THREAD_CREATE_FAIL;
    }
#else
    if (unlikely(pthread_mutex_init(&syncp->sync_mutex, nullptr))) {
      goto InitRLstreamEx_ret_THREAD_CREATE_FAIL;
    }
    rlsp->sync_init_state = 1;
    if (unlikely(pthread_cond_init(&syncp->reader_progress_condvar, nullptr))) {
      goto InitRLstreamEx_ret_THREAD_CREATE_FAIL;
    }
    rlsp->sync_init_state = 2;
    syncp->consumer_progress_state = 0;
    if (unlikely(pthread_cond_init(&syncp->consumer_progress_condvar, nullptr))) {
      goto InitRLstreamEx_ret_THREAD_CREATE_FAIL;
    }
    rlsp->sync_init_state = 3;
    if (unlikely(pthread_create(&rlsp->read_thread, &g_smallstack_thread_attr, ReadLineStreamThread, rlsp))) {
      goto InitRLstreamEx_ret_THREAD_CREATE_FAIL;
    }
    rlsp->sync_init_state = 4;
#endif
    *consume_iterp = &(buf[-1]);
  }
  while (0) {
  InitRLstreamEx_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  InitRLstreamEx_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
  return reterr;
}

PglErr AdvanceRLstream(ReadLineStream* rlsp, char** consume_iterp) {
  char* consume_iter = *consume_iterp;
  ReadLineStreamSync* syncp = rlsp->syncp;
#ifdef _WIN32
  CRITICAL_SECTION* critical_sectionp = &syncp->critical_section;
  HANDLE consumer_progress_event = rlsp->consumer_progress_event;
  while (1) {
    EnterCriticalSection(critical_sectionp);
    const PglErr reterr = syncp->reterr;
    if (unlikely((reterr != kPglRetSuccess) && (reterr != kPglRetEof))) {
      LeaveCriticalSection(critical_sectionp);
      // No need to set consumer_progress event here, just let the cleanup
      // routine take care of that.
      return reterr;
    }
    char* available_end = syncp->available_end;
    char* cur_circular_end = syncp->cur_circular_end;
    if (consume_iter == cur_circular_end) {
      char* buf = rlsp->buf;
      consume_iter = buf;
      cur_circular_end = nullptr;
      syncp->cur_circular_end = nullptr;
      *consume_iterp = buf;
      if (consume_iter != available_end) {
        SetEvent(consumer_progress_event);
      }
    }
    syncp->consume_tail = consume_iter;
    if ((consume_iter != available_end) || cur_circular_end) {
      if (cur_circular_end) {
        rlsp->consume_stop = cur_circular_end;
      } else {
        rlsp->consume_stop = available_end;
      }
      LeaveCriticalSection(critical_sectionp);
      // We could set the consumer_progress event here, but it's not really
      // necessary?
      // SetEvent(consumer_progress_event);
      return kPglRetSuccess;
    }
    SetEvent(consumer_progress_event);
    LeaveCriticalSection(critical_sectionp);
    // We've processed all the consume-ready bytes...
    if (reterr != kPglRetSuccess) {
      // ...and we're at eof.  Don't set consumer_progress event here; let that
      // wait until cleanup or rewind/retarget.
      return reterr;
    }
    // ...and there's probably more.
    WaitForSingleObject(rlsp->reader_progress_event, INFINITE);
    // bugfix (2 Oct 2018)
    consume_iter = syncp->consume_tail;
    *consume_iterp = syncp->consume_tail;
  }
#else
  pthread_mutex_t* sync_mutexp = &syncp->sync_mutex;
  pthread_cond_t* consumer_progress_condvarp = &syncp->consumer_progress_condvar;
  pthread_cond_t* reader_progress_condvarp = &syncp->reader_progress_condvar;
  pthread_mutex_lock(sync_mutexp);
  while (1) {
    const PglErr reterr = syncp->reterr;
    if (unlikely((reterr != kPglRetSuccess) && (reterr != kPglRetEof))) {
      pthread_mutex_unlock(sync_mutexp);
      return reterr;
    }
    char* available_end = syncp->available_end;
    // bugfix (2 Oct 2018): There were TWO consume_iter == available_end ==
    // cur_circular_end cases.
    // printf("checking for more to consume: %lx %lx %lx\n", (uintptr_t)consume_iter, (uintptr_t)syncp->cur_circular_end, (uintptr_t)available_end);
    if (consume_iter == syncp->cur_circular_end) {
      char* buf = rlsp->buf;
      consume_iter = buf;
      *consume_iterp = buf;
      syncp->cur_circular_end = nullptr;
      // File-reader could be waiting on either "all bytes in front have been
      // consumed, some bytes behind may remain" or "all bytes have been
      // consumed".  Signal in case it's the first.
      if (consume_iter != available_end) {
        syncp->consumer_progress_state = 1;
        pthread_cond_signal(consumer_progress_condvarp);
      }
    }
    syncp->consume_tail = consume_iter;
    // If cur_circular_end is still non-null here, there must be bytes
    // available even when consume_iter == available_end.  (Is the latter
    // still possible?  Check this.)
    if ((consume_iter != available_end) || syncp->cur_circular_end) {
      // assert(consume_iter != available_end);
      if (syncp->cur_circular_end) {
        rlsp->consume_stop = syncp->cur_circular_end;
      } else {
        rlsp->consume_stop = available_end;
      }
      // pthread_cond_signal(consumer_progress_condvarp);
      pthread_mutex_unlock(sync_mutexp);
      // printf("consuming %lx..%lx\n", (uintptr_t)(*consume_iterp), (uintptr_t)rlsp->consume_stop);
      return kPglRetSuccess;
    }
    // We've processed all the consume-ready bytes...
    if (reterr != kPglRetSuccess) {
      // ...and we're at eof.
      pthread_mutex_unlock(sync_mutexp);
      return reterr;
    }
    // ...and there's probably more.
    syncp->consumer_progress_state = 1;
    pthread_cond_signal(consumer_progress_condvarp);
    // no need for an explicit spurious-wakeup check, we'll check the progress
    // condition (available_end has advanced, or we have a read error) anyway
    // and get back here if it isn't satisfied
    pthread_cond_wait(reader_progress_condvarp, sync_mutexp);
    // bugfix (2 Oct 2018)
    consume_iter = syncp->consume_tail;
    *consume_iterp = syncp->consume_tail;
  }
#endif
}

PglErr RlsNextNonemptyLstrip(ReadLineStream* rlsp, uintptr_t* line_idx_ptr, char** consume_iterp) {
  uintptr_t line_idx = *line_idx_ptr;
  char* consume_iter = *consume_iterp;
  do {
    ++line_idx;
    consume_iter = AdvPastDelim(consume_iter, '\n');
    PglErr reterr = RlsPostlfNext(rlsp, &consume_iter);
    // not unlikely() due to eof
    if (reterr) {
      return reterr;
    }
    consume_iter = FirstNonTspace(consume_iter);
  } while (IsEolnKns(*consume_iter));
  *line_idx_ptr = line_idx;
  *consume_iterp = consume_iter;
  return kPglRetSuccess;
}

PglErr RlsSkipNz(uintptr_t skip_ct, ReadLineStream* rlsp, char** consume_iterp) {
#ifdef __LP64__
  char* consume_iter = *consume_iterp;
  // Minor extension of AdvToNthDelimChecked().
  const VecUc vvec_all_lf = vecuc_set1('\n');
  while (1) {
    uintptr_t starting_addr = R_CAST(uintptr_t, consume_iter);
    VecUc* consume_viter = R_CAST(VecUc*, RoundDownPow2(starting_addr, kBytesPerVec));
    uintptr_t ending_addr = R_CAST(uintptr_t, rlsp->consume_stop);
    VecUc* consume_vstop = R_CAST(VecUc*, RoundDownPow2(ending_addr, kBytesPerVec));
    VecUc cur_vvec = *consume_viter;
    VecUc lf_vvec = (cur_vvec == vvec_all_lf);
    uint32_t lf_bytes = vecuc_movemask(lf_vvec);
    const uint32_t leading_byte_ct = starting_addr - R_CAST(uintptr_t, consume_viter);
    const uint32_t leading_mask = UINT32_MAX << leading_byte_ct;
    lf_bytes &= leading_mask;
    uint32_t cur_lf_ct;
    for (; consume_viter != consume_vstop; ++consume_viter) {
      cur_lf_ct = PopcountVec8thUint(lf_bytes);
      if (cur_lf_ct >= skip_ct) {
        goto SkipNextLineInRLstreamRaw_finish;
      }
      skip_ct -= cur_lf_ct;
    }
    lf_bytes &= (1U << (ending_addr % kBytesPerVec)) - 1;
    cur_lf_ct = PopcountVec8thUint(lf_bytes);
    if (cur_lf_ct > skip_ct) {
    SkipNextLineInRLstreamRaw_finish:
      lf_bytes = ClearBottomSetBits(skip_ct, cur_lf_ct);
      const uint32_t byte_offset_in_vec = ctzu32(lf_bytes);
      const uintptr_t result_addr = R_CAST(uintptr_t, consume_viter) + byte_offset_in_vec;
      // return last character in last skipped line
      *consume_iterp = R_CAST(char*, result_addr);
      return kPglRetSuccess;
    }
    skip_ct -= cur_lf_ct;
    consume_iter = rlsp->consume_stop;
    PglErr reterr = AdvanceRLstream(rlsp, &consume_iter);
    // not unlikely() due to eof
    if (reterr) {
      return reterr;
    }
  }
#else
  char* consume_iter = *consume_iterp;
  char* consume_stop = rlsp->consume_stop;
  for (uintptr_t ulii = 0; ulii != skip_ct; ++ulii) {
    consume_iter = AdvPastDelim(consume_iter, '\n');
    if (consume_iter == consume_stop) {
      PglErr reterr = AdvanceRLstream(rlsp, &consume_iter);
      if (reterr) {
        return reterr;
      }
      consume_stop = rlsp->consume_stop;
    }
  }
  // return beginning of last skipped line
  *consume_iterp = consume_iter;
  return kPglRetSuccess;
#endif
}

PglErr RetargetRLstreamRaw(const char* new_fname, ReadLineStream* rlsp, char** consume_iterp) {
  char* buf = rlsp->buf;
  ReadLineStreamSync* syncp = rlsp->syncp;
#ifdef _WIN32
  CRITICAL_SECTION* critical_sectionp = &syncp->critical_section;
  EnterCriticalSection(critical_sectionp);
  const PglErr reterr = syncp->reterr;
  if (reterr != kPglRetSuccess) {
    if (unlikely(reterr != kPglRetEof)) {
      LeaveCriticalSection(critical_sectionp);
      return reterr;
    }
    // clear eof
    syncp->reterr = kPglRetSuccess;
  }
  // bugfix (5 Mar 2018): need to reset these here, can't wait for reader
  // thread to receive signal
  syncp->consume_tail = buf;
  syncp->cur_circular_end = nullptr;
  syncp->available_end = buf;

  syncp->interrupt = kRlsInterruptRetarget;
  // Could also just open the file in this function (before acquiring the
  // mutex) and pass a gzFile.  Advantages: nothing bad happens if new_fname
  // is overwritten before it's read, RLstreamErrPrint() no longer has to deal
  // with OpenFail error.  Disadvantage: peak resource usage is a bit higher if
  // we open the second file before closing the first one.  Advantages probably
  // outweigh disadvantages, but I'll wait till --pmerge development to make a
  // decision since that's the main function that actually cares.
  syncp->new_fname = new_fname;
  SetEvent(rlsp->consumer_progress_event);
  LeaveCriticalSection(critical_sectionp);
#else
  pthread_mutex_t* sync_mutexp = &syncp->sync_mutex;
  pthread_cond_t* consumer_progress_condvarp = &syncp->consumer_progress_condvar;
  pthread_mutex_lock(sync_mutexp);
  const PglErr reterr = syncp->reterr;
  if (reterr != kPglRetSuccess) {
    if (unlikely(reterr != kPglRetEof)) {
      pthread_mutex_unlock(sync_mutexp);
      return reterr;
    }
    // clear eof
    syncp->reterr = kPglRetSuccess;
  }
  syncp->consume_tail = buf;
  syncp->cur_circular_end = nullptr;
  syncp->available_end = buf;
  syncp->interrupt = kRlsInterruptRetarget;
  syncp->new_fname = new_fname;
  syncp->consumer_progress_state = 1;
  pthread_cond_signal(consumer_progress_condvarp);
  pthread_mutex_unlock(sync_mutexp);
#endif
  *consume_iterp = &(buf[-1]);
  rlsp->consume_stop = buf;
  return kPglRetSuccess;
}

PglErr CleanupRLstream(ReadLineStream* rlsp) {
  PglErr reterr = kPglRetSuccess;
#ifdef _WIN32
  if (rlsp->read_thread) {
    ReadLineStreamSync* syncp = rlsp->syncp;
    CRITICAL_SECTION* critical_sectionp = &syncp->critical_section;
    EnterCriticalSection(critical_sectionp);
    syncp->interrupt = kRlsInterruptShutdown;
    SetEvent(rlsp->consumer_progress_event);
    LeaveCriticalSection(critical_sectionp);
    WaitForSingleObject(rlsp->read_thread, INFINITE);
    DeleteCriticalSection(critical_sectionp);
    rlsp->read_thread = nullptr;  // make it safe to call this multiple times
    CloseHandle(rlsp->consumer_progress_event);
    CloseHandle(rlsp->reader_progress_event);
  }
#else
  const uint32_t sync_init_state = rlsp->sync_init_state;
  if (sync_init_state) {
    ReadLineStreamSync* syncp = rlsp->syncp;
    pthread_mutex_t* sync_mutexp = &syncp->sync_mutex;
    pthread_cond_t* consumer_progress_condvarp = &syncp->consumer_progress_condvar;
    if (sync_init_state == 4) {
      pthread_mutex_lock(sync_mutexp);
      syncp->interrupt = kRlsInterruptShutdown;
      syncp->consumer_progress_state = 1;
      pthread_cond_signal(consumer_progress_condvarp);
      pthread_mutex_unlock(sync_mutexp);
      pthread_join(rlsp->read_thread, nullptr);
    }
    pthread_mutex_destroy(sync_mutexp);
    if (sync_init_state > 1) {
      pthread_cond_destroy(&syncp->reader_progress_condvar);
      if (sync_init_state > 2) {
        pthread_cond_destroy(consumer_progress_condvarp);
      }
    }
    rlsp->sync_init_state = 0;  // make it safe to call this multiple times
  }
#endif
  if (rlsp->gz_infile) {
    if (unlikely(gzclose_null(&rlsp->gz_infile))) {
      reterr = kPglRetReadFail;
    }
  } else if (rlsp->bgz_infile) {
    if (unlikely(bgzf_close(rlsp->bgz_infile))) {
      reterr = kPglRetReadFail;
    }
    rlsp->bgz_infile = nullptr;
  }
  return reterr;
}

void RLstreamErrPrint(const char* file_descrip, ReadLineStream* rlsp, PglErr* reterr_ptr) {
  if (*reterr_ptr != kPglRetLongLine) {
    if (*reterr_ptr == kPglRetOpenFail) {
      putc_unlocked('\n', stdout);
      logerrprintfww(kErrprintfFopen, rlsp->syncp->new_fname);
    }
    return;
  }
  if (S_CAST(uintptr_t, rlsp->buf_end - rlsp->buf) != rlsp->enforced_max_line_blen + kDecompressChunkSize) {
    *reterr_ptr = kPglRetNomem;
    return;
  }
  // Could report accurate line number, but probably not worth the trouble.
  putc_unlocked('\n', stdout);
  logerrprintfww("Error: Pathologically long line in %s.\n", file_descrip);
  *reterr_ptr = kPglRetMalformedInput;
}

#ifdef __cplusplus
}
#endif
