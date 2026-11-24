// Native cross-platform PDF metadata extraction and page rasterization:
// - macOS (__APPLE__): CoreGraphics + ImageIO (zero external tool dependencies)
// - Linux (!__APPLE__): fork()+execv() auto-detection of pdftoppm -> mutool -> gs
//   (no shell; unique temp dir cleaned immediately; returns empty bytes if no tool installed).
#include <moonbit.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
__asm__(".linker_option \"-framework\", \"CoreGraphics\"\n"
        ".linker_option \"-framework\", \"CoreFoundation\"\n"
        ".linker_option \"-framework\", \"ImageIO\"\n");

static CGPDFDocumentRef open_pdf_doc(moonbit_bytes_t pdf_bytes) {
  int32_t len = Moonbit_array_length(pdf_bytes);
  if (len < 8) return NULL;
  CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, (const void*)pdf_bytes, (size_t)len, NULL);
  if (!prov) return NULL;
  CGPDFDocumentRef doc = CGPDFDocumentCreateWithProvider(prov);
  CGDataProviderRelease(prov);
  return doc;
}

int32_t fv_pdf_page_count(moonbit_bytes_t pdf_bytes) {
  CGPDFDocumentRef doc = open_pdf_doc(pdf_bytes);
  if (!doc) return 0;
  size_t n = CGPDFDocumentGetNumberOfPages(doc);
  CGPDFDocumentRelease(doc);
  return (int32_t)n;
}

moonbit_bytes_t fv_pdf_title(moonbit_bytes_t pdf_bytes) {
  moonbit_bytes_t empty = moonbit_make_bytes(0, 0);
  CGPDFDocumentRef doc = open_pdf_doc(pdf_bytes);
  if (!doc) return empty;
  CGPDFDictionaryRef info = CGPDFDocumentGetInfo(doc);
  if (!info) {
    CGPDFDocumentRelease(doc);
    return empty;
  }
  CGPDFStringRef pdf_str = NULL;
  if (!CGPDFDictionaryGetString(info, "Title", &pdf_str) || !pdf_str) {
    CGPDFDocumentRelease(doc);
    return empty;
  }
  CFStringRef cf_str = CGPDFStringCopyTextString(pdf_str);
  CGPDFDocumentRelease(doc);
  if (!cf_str) return empty;
  CFIndex max_len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(cf_str), kCFStringEncodingUTF8) + 1;
  char* buf = (char*)malloc((size_t)max_len);
  moonbit_bytes_t out = empty;
  if (buf && CFStringGetCString(cf_str, buf, max_len, kCFStringEncodingUTF8)) {
    size_t slen = strlen(buf);
    out = moonbit_make_bytes((int32_t)slen, 0);
    if (slen > 0) memcpy(out, buf, slen);
  }
  if (buf) free(buf);
  CFRelease(cf_str);
  return out;
}

moonbit_bytes_t fv_pdf_render_page(moonbit_bytes_t pdf_bytes, int32_t page_1based, int32_t dpi) {
  moonbit_bytes_t empty = moonbit_make_bytes(0, 0);
  if (page_1based < 1) return empty;
  if (dpi <= 0) dpi = 144;
  CGPDFDocumentRef doc = open_pdf_doc(pdf_bytes);
  if (!doc) return empty;
  size_t n = CGPDFDocumentGetNumberOfPages(doc);
  if ((size_t)page_1based > n) {
    CGPDFDocumentRelease(doc);
    return empty;
  }
  CGPDFPageRef page = CGPDFDocumentGetPage(doc, (size_t)page_1based);
  if (!page) {
    CGPDFDocumentRelease(doc);
    return empty;
  }
  CGRect box = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
  if (box.size.width <= 0 || box.size.height <= 0) {
    CGPDFDocumentRelease(doc);
    return empty;
  }
  int rot = CGPDFPageGetRotationAngle(page) % 360;
  if (rot < 0) rot += 360;
  double bw = (rot == 90 || rot == 270) ? box.size.height : box.size.width;
  double bh = (rot == 90 || rot == 270) ? box.size.width : box.size.height;
  double scale = (double)dpi / 72.0;
  double pw_d = bw * scale;
  double ph_d = bh * scale;
  double max_dim = pw_d > ph_d ? pw_d : ph_d;
  if (max_dim > 1600.0) {
    double shrink = 1600.0 / max_dim;
    pw_d *= shrink;
    ph_d *= shrink;
  }
  int pw = (int)(pw_d + 0.5);
  int ph = (int)(ph_d + 0.5);
  if (pw < 1) pw = 1;
  if (ph < 1) ph = 1;

  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      NULL, (size_t)pw, (size_t)ph, 8, (size_t)pw * 4, cs,
      (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault);
  CGColorSpaceRelease(cs);
  if (!ctx) {
    CGPDFDocumentRelease(doc);
    return empty;
  }
  CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
  CGContextFillRect(ctx, CGRectMake(0, 0, pw, ph));
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  // CGPDFPageGetDrawingTransform never scales up beyond 1:1, so scale CTM to logical page
  // dimensions (bw x bh) first, then let CoreGraphics handle origin offset and /Rotate cleanly.
  CGContextScaleCTM(ctx, (double)pw / bw, (double)ph / bh);
  CGAffineTransform xform = CGPDFPageGetDrawingTransform(
      page, kCGPDFMediaBox, CGRectMake(0, 0, bw, bh), 0, true);
  CGContextConcatCTM(ctx, xform);
  CGContextDrawPDFPage(ctx, page);
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  CGContextRelease(ctx);
  CGPDFDocumentRelease(doc);
  if (!img) return empty;

  CFMutableDataRef enc = CFDataCreateMutable(NULL, 0);
  CGImageDestinationRef dst = CGImageDestinationCreateWithData(enc, CFSTR("public.jpeg"), 1, NULL);
  moonbit_bytes_t out = empty;
  if (dst) {
    float q = 0.85f;
    CFNumberRef qn = CFNumberCreate(NULL, kCFNumberFloatType, &q);
    const void* k[] = { kCGImageDestinationLossyCompressionQuality };
    const void* v[] = { qn };
    CFDictionaryRef opts = CFDictionaryCreate(
        NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(qn);
    CGImageDestinationAddImage(dst, img, opts);
    if (opts) CFRelease(opts);
    if (CGImageDestinationFinalize(dst)) {
      CFIndex len = CFDataGetLength(enc);
      out = moonbit_make_bytes((int32_t)len, 0);
      if (len > 0) memcpy(out, CFDataGetBytePtr(enc), (size_t)len);
    }
    CFRelease(dst);
  }
  CFRelease(enc);
  CGImageRelease(img);
  return out;
}

#else

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int pdf_resolve_bin(const char* bin, char* out, size_t cap) {
  const char* path = getenv("PATH");
  if (path) {
    const char* p = path;
    while (*p) {
      const char* colon = strchr(p, ':');
      size_t len = colon ? (size_t)(colon - p) : strlen(p);
      if (len > 0 && len + strlen(bin) + 2 < cap) {
        memcpy(out, p, len);
        out[len] = '/';
        strcpy(out + len + 1, bin);
        if (access(out, X_OK) == 0) return 1;
      }
      if (!colon) break;
      p = colon + 1;
    }
  }
  const char* fallbacks[] = { "/usr/bin", "/usr/local/bin", NULL };
  for (int i = 0; fallbacks[i]; i++) {
    snprintf(out, cap, "%s/%s", fallbacks[i], bin);
    if (access(out, X_OK) == 0) return 1;
  }
  return 0;
}

static void pdf_rm_dir(const char* dir) {
  DIR* d = opendir(dir);
  if (d) {
    struct dirent* e;
    char p[512];
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      unlink(p);
    }
    closedir(d);
  }
  rmdir(dir);
}

static int pdf_write_tmp(moonbit_bytes_t pdf_bytes, char* dir_out, size_t dir_cap, char* file_out, size_t file_cap) {
  int32_t len = Moonbit_array_length(pdf_bytes);
  if (len < 8) return 0;
  snprintf(dir_out, dir_cap, "/tmp/fv-pdf-XXXXXX");
  if (!mkdtemp(dir_out)) return 0;
  snprintf(file_out, file_cap, "%s/doc.pdf", dir_out);
  int fd = open(file_out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    pdf_rm_dir(dir_out);
    return 0;
  }
  size_t off = 0;
  while (off < (size_t)len) {
    ssize_t w = write(fd, (const uint8_t*)pdf_bytes + off, (size_t)len - off);
    if (w <= 0) {
      if (errno == EINTR) continue;
      close(fd);
      pdf_rm_dir(dir_out);
      return 0;
    }
    off += (size_t)w;
  }
  close(fd);
  return 1;
}

static int pdf_run_capture(const char* exe, char* const argv[], char* out, size_t out_cap) {
  int pfd[2];
  if (pipe(pfd) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    close(pfd[1]);
    execv(exe, argv);
    _exit(127);
  }
  close(pfd[1]);
  if (pid < 0) {
    close(pfd[0]);
    return -1;
  }
  size_t total = 0;
  while (out && total + 1 < out_cap) {
    ssize_t r = read(pfd[0], out + total, out_cap - 1 - total);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) break;
    total += (size_t)r;
  }
  if (out && out_cap > 0) out[total] = 0;
  close(pfd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? (int)total : -1;
}

static int32_t scan_pdf_pages_fallback(const uint8_t* data, size_t len) {
  int32_t count = 0;
  for (size_t i = 0; i + 10 < len; i++) {
    if (data[i] == '/' && memcmp(data + i, "/Type", 5) == 0) {
      size_t j = i + 5;
      while (j < len && (data[j] == ' ' || data[j] == '\t' || data[j] == '\r' || data[j] == '\n')) j++;
      if (j + 5 <= len && memcmp(data + j, "/Page", 5) == 0) {
        uint8_t next = (j + 5 < len) ? data[j + 5] : ' ';
        if (next != 's' && !(next >= 'A' && next <= 'Z') && !(next >= 'a' && next <= 'z')) {
          count++;
        }
      }
    }
  }
  return count;
}

int32_t fv_pdf_page_count(moonbit_bytes_t pdf_bytes) {
  int32_t len = Moonbit_array_length(pdf_bytes);
  if (len < 8) return 0;
  char bin[512];
  if (pdf_resolve_bin("pdfinfo", bin, sizeof(bin))) {
    char dir[256], file[512];
    if (pdf_write_tmp(pdf_bytes, dir, sizeof(dir), file, sizeof(file))) {
      char out[4096];
      char* argv[] = { bin, file, NULL };
      int n = pdf_run_capture(bin, argv, out, sizeof(out));
      pdf_rm_dir(dir);
      if (n > 0) {
        const char* p = strstr(out, "Pages:");
        if (p) {
          int pages = atoi(p + 6);
          if (pages > 0) return (int32_t)pages;
        }
      }
    }
  }
  return scan_pdf_pages_fallback((const uint8_t*)pdf_bytes, (size_t)len);
}

moonbit_bytes_t fv_pdf_title(moonbit_bytes_t pdf_bytes) {
  moonbit_bytes_t empty = moonbit_make_bytes(0, 0);
  char bin[512];
  if (!pdf_resolve_bin("pdfinfo", bin, sizeof(bin))) return empty;
  char dir[256], file[512];
  if (!pdf_write_tmp(pdf_bytes, dir, sizeof(dir), file, sizeof(file))) return empty;
  char out[4096];
  char* argv[] = { bin, file, NULL };
  int n = pdf_run_capture(bin, argv, out, sizeof(out));
  pdf_rm_dir(dir);
  if (n <= 0) return empty;
  const char* p = strstr(out, "Title:");
  if (!p) return empty;
  p += 6;
  while (*p == ' ' || *p == '\t') p++;
  const char* end = strchr(p, '\n');
  size_t slen = end ? (size_t)(end - p) : strlen(p);
  while (slen > 0 && (p[slen - 1] == '\r' || p[slen - 1] == ' ')) slen--;
  moonbit_bytes_t res = moonbit_make_bytes((int32_t)slen, 0);
  if (slen > 0) memcpy(res, p, slen);
  return res;
}

static moonbit_bytes_t read_file_bytes(const char* path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return moonbit_make_bytes(0, 0);
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 32 * 1024 * 1024) {
    close(fd);
    return moonbit_make_bytes(0, 0);
  }
  moonbit_bytes_t res = moonbit_make_bytes((int32_t)st.st_size, 0);
  size_t off = 0;
  while (off < (size_t)st.st_size) {
    ssize_t r = read(fd, (uint8_t*)res + off, (size_t)st.st_size - off);
    if (r <= 0) {
      if (r < 0 && errno == EINTR) continue;
      break;
    }
    off += (size_t)r;
  }
  close(fd);
  return res;
}

moonbit_bytes_t fv_pdf_render_page(moonbit_bytes_t pdf_bytes, int32_t page_1based, int32_t dpi) {
  if (page_1based < 1) return moonbit_make_bytes(0, 0);
  if (dpi <= 0) dpi = 144;
  char dir[256], file[512], out_prefix[512], out_jpg[512];
  if (!pdf_write_tmp(pdf_bytes, dir, sizeof(dir), file, sizeof(file))) {
    return moonbit_make_bytes(0, 0);
  }
  snprintf(out_prefix, sizeof(out_prefix), "%s/page", dir);
  snprintf(out_jpg, sizeof(out_jpg), "%s/page.jpg", dir);
  char dpi_s[16], page_s[16], r_arg[32], f_arg[32], l_arg[32], o_arg[544];
  snprintf(dpi_s, sizeof(dpi_s), "%d", dpi);
  snprintf(page_s, sizeof(page_s), "%d", page_1based);

  char bin[512];
  moonbit_bytes_t res = moonbit_make_bytes(0, 0);
  if (pdf_resolve_bin("pdftoppm", bin, sizeof(bin))) {
    char* argv[] = {
      bin, "-jpeg", "-r", dpi_s, "-scale-to", "1600",
      "-f", page_s, "-l", page_s, "-singlefile",
      file, out_prefix, NULL
    };
    if (pdf_run_capture(bin, argv, NULL, 0) >= 0) {
      res = read_file_bytes(out_jpg);
    }
  } else if (pdf_resolve_bin("mutool", bin, sizeof(bin))) {
    char* argv[] = {
      bin, "draw", "-F", "jpeg", "-r", dpi_s, "-o", out_jpg, file, page_s, NULL
    };
    if (pdf_run_capture(bin, argv, NULL, 0) >= 0) {
      res = read_file_bytes(out_jpg);
    }
  } else if (pdf_resolve_bin("gs", bin, sizeof(bin))) {
    snprintf(r_arg, sizeof(r_arg), "-r%d", dpi);
    snprintf(f_arg, sizeof(f_arg), "-dFirstPage=%d", page_1based);
    snprintf(l_arg, sizeof(l_arg), "-dLastPage=%d", page_1based);
    snprintf(o_arg, sizeof(o_arg), "-sOutputFile=%s", out_jpg);
    char* argv[] = {
      bin, "-q", "-dNOPAUSE", "-dBATCH", "-sDEVICE=jpeg", r_arg, f_arg, l_arg, o_arg, file, NULL
    };
    if (pdf_run_capture(bin, argv, NULL, 0) >= 0) {
      res = read_file_bytes(out_jpg);
    }
  }
  pdf_rm_dir(dir);
  return res;
}
#endif
