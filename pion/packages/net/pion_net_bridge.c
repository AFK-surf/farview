#include "pion_rt.h"
#include <stdint.h>
#include <dirent.h>

extern int32_t fv_poll(int32_t *fds, int32_t n, int32_t wait_ms);
extern int32_t fv_pipe_open(int32_t *out_fds);
extern int32_t fv_poll_fds(int32_t *fds, int32_t *events, int32_t *revents, int32_t n, int32_t wait_ms);

const char *pion_dirent_name(void *ent) {
  return ent ? ((struct dirent *)ent)->d_name : "";
}

int32_t pion_fv_poll(void *fds_i32, int32_t n, int32_t wait_ms) {
  int32_t tmp[64];
  if (n > 64) n = 64;
  if (n < 0) n = 0;
  const int32_t *src = (const int32_t *)fds_i32;
  for (int32_t i = 0; i < n; i++) {
    tmp[i] = src[i];
  }
  return fv_poll(tmp, n, wait_ms);
}

int32_t pion_fv_pipe_open(void *out_i32) {
  return fv_pipe_open((int32_t *)out_i32);
}

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

extern int32_t fv_set_nonblock(int32_t fd, int32_t nonblock);
extern int32_t fv_pipe_wake(int32_t write_fd);
extern int32_t fv_pipe_drain(int32_t read_fd);
extern int32_t fv_send_nb(int32_t fd, const uint8_t *b, int32_t off, int32_t len);
extern int32_t fv_recv_nb(int32_t fd, uint8_t *b, int32_t off, int32_t cap);
extern void fv_close(int32_t fd);

int32_t pion_fv_poll_fds(void *fds_i32, void *events_i32, void *revents_i32, int32_t n, int32_t wait_ms) {
  return fv_poll_fds((int32_t *)fds_i32, (int32_t *)events_i32, (int32_t *)revents_i32, n, wait_ms);
}

typedef struct {
  int32_t wake_r;
  int32_t sock_fd;
  int32_t arm_mask;
  int32_t stop;
  int32_t wakes;
  int32_t ready_signalled;
  pthread_mutex_t mu;
  pthread_cond_t cv;
} MtPollCtx;

static void *mt_poll_thread(void *arg) {
  MtPollCtx *c = (MtPollCtx *)arg;
  for (;;) {
    pthread_mutex_lock(&c->mu);
    int32_t stop = c->stop;
    int32_t mask = c->arm_mask;
    pthread_mutex_unlock(&c->mu);
    if (stop) break;

    int32_t fds[2] = { c->wake_r, c->sock_fd };
    int32_t evs[2] = { 1, mask };
    int32_t revs[2] = { 0, 0 };
    int32_t n = mask != 0 ? 2 : 1;
    int32_t rc = fv_poll_fds(fds, evs, revs, n, -1);
    if (rc == -3) continue;
    if (rc < 0) break;
    if (revs[0] & 1) (void)fv_pipe_drain(c->wake_r);
    if (n == 2 && revs[1] != 0) {
      pthread_mutex_lock(&c->mu);
      if (revs[1] & 1) c->arm_mask &= ~1;
      if (revs[1] & 2) c->arm_mask &= ~2;
      c->wakes++;
      c->ready_signalled = 1;
      pthread_cond_signal(&c->cv);
      pthread_mutex_unlock(&c->mu);
    }
  }
  return NULL;
}

// Multi-threaded OS-thread stress for fv_poll_fds + fv_pipe_open/wake/drain + disarm/re-arm POLLIN->POLLOUT.
// Returns 0 on full pass, non-zero error code on failure.
int32_t pion_fv_mt_poll_stress(int32_t cli_fd, int32_t acc_fd, int32_t rounds) {
  int32_t pipe_fds[2] = { -1, -1 };
  if (fv_pipe_open(pipe_fds) != 0) return 1;
  if (fv_set_nonblock(cli_fd, 1) != 0 || fv_set_nonblock(acc_fd, 1) != 0) {
    fv_close(pipe_fds[0]);
    fv_close(pipe_fds[1]);
    return 2;
  }
  MtPollCtx ctx;
  ctx.wake_r = pipe_fds[0];
  ctx.sock_fd = cli_fd;
  ctx.arm_mask = 1; // POLL_IN
  ctx.stop = 0;
  ctx.wakes = 0;
  ctx.ready_signalled = 0;
  pthread_mutex_init(&ctx.mu, NULL);
  pthread_cond_init(&ctx.cv, NULL);

  pthread_t th;
  if (pthread_create(&th, NULL, mt_poll_thread, &ctx) != 0) {
    fv_close(pipe_fds[0]);
    fv_close(pipe_fds[1]);
    return 3;
  }

  uint8_t tx = 0x5A;
  uint8_t rx[16];
  int32_t err = 0;
  for (int32_t r = 0; r < rounds; r++) {
    if (fv_send_nb(acc_fd, &tx, 0, 1) != 1) { err = 4; break; }
    pthread_mutex_lock(&ctx.mu);
    while (!ctx.ready_signalled) pthread_cond_wait(&ctx.cv, &ctx.mu);
    ctx.ready_signalled = 0;
    pthread_mutex_unlock(&ctx.mu);

    int32_t k = fv_recv_nb(cli_fd, rx, 0, 16);
    if (k != 1 || rx[0] != tx) { err = 5; break; }

    // Re-arm POLL_IN for next round
    pthread_mutex_lock(&ctx.mu);
    ctx.arm_mask = 1;
    (void)fv_pipe_wake(pipe_fds[1]);
    pthread_mutex_unlock(&ctx.mu);
  }

  // Also test re-arming POLL_OUT (2) and waking cleanly
  if (err == 0) {
    pthread_mutex_lock(&ctx.mu);
    ctx.arm_mask = 2;
    (void)fv_pipe_wake(pipe_fds[1]);
    while (!ctx.ready_signalled) pthread_cond_wait(&ctx.cv, &ctx.mu);
    ctx.ready_signalled = 0;
    pthread_mutex_unlock(&ctx.mu);
  }

  pthread_mutex_lock(&ctx.mu);
  ctx.stop = 1;
  (void)fv_pipe_wake(pipe_fds[1]);
  pthread_mutex_unlock(&ctx.mu);
  pthread_join(th, NULL);

  pthread_cond_destroy(&ctx.cv);
  pthread_mutex_destroy(&ctx.mu);
  fv_close(pipe_fds[0]);
  fv_close(pipe_fds[1]);
  return err;
}
