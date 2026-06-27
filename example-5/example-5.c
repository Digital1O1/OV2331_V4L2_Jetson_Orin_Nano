/**
 * Video for Linux version 2 (V4L2) example 5 - video capture
 *
 * Based on
 * - https://linuxtv.org/downloads/v4l-dvb-apis/uapi/v4l/capture.c.html
 *
 * Kyle M. Douglass, 2018
 * kyle.m.douglass@gmail.com
 */
#include <sys/ioctl.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <opencv2/opencv.hpp>

static const char DEVICE[] = "/dev/video0";
static const int SENSOR_WIDTH  = 1300;
static const int SENSOR_HEIGHT = 1600;

// Desired exposure value — adjust this to tune brightness.
// Must be <= (SENSOR_HEIGHT + vertical_blanking - margin).
// Open the blanking window first via set_vertical_blanking() if needed.
static const int DESIRED_EXPOSURE   = 100;
static const int DESIRED_VBLANKING  = 5000;  // Must be >= 174 (driver min)

int fd;
struct {
  void *start;
  size_t length;
} *buffers;
unsigned int num_buffers;
struct v4l2_requestbuffers reqbuf = {0};

/**
 * Wrapper around ioctl calls.
 */
static int xioctl(int fd, int request, void *arg) {
  int r;

  do {
    r = ioctl(fd, request, arg);
  } while (-1 == r && EINTR == errno);

  return r;
}

/**
 * Helper: set a V4L2 integer control and read it back for verification.
 * Returns the value the driver actually accepted, or -1 on error.
 */
static int set_ctrl_verified(int ctrl_id, int value, const char *name) {
  struct v4l2_control ctrl = {};
  ctrl.id    = ctrl_id;
  ctrl.value = value;

  if (-1 == xioctl(fd, VIDIOC_S_CTRL, &ctrl)) {
    perror(name);
    return -1;
  }

  struct v4l2_control readback = {};
  readback.id = ctrl_id;

  if (0 == xioctl(fd, VIDIOC_G_CTRL, &readback)) {
    std::cout << name << " after write = " << readback.value << std::endl;
    return readback.value;
  } else {
    perror("VIDIOC_G_CTRL readback");
    return -1;
  }
}

/**
 * Set vertical blanking to open the exposure window before setting exposure.
 *
 * On OV2311 the maximum exposure is bounded by:
 *   frame_height + vblank - some_sensor_offset
 * So this must be called BEFORE set_exposure() when targeting high values.
 *
 * Control ID: 0x009e0901 (V4L2_CID_VERTICAL_BLANKING / Image Source Controls)
 * Driver range: min=174, max=16399
 */
static void set_vertical_blanking(int vblank) {
  set_ctrl_verified(V4L2_CID_VBLANK, vblank, "vertical_blanking");
}

/**
 * Set sensor exposure.
 *
 * On the Tegra VI/ISP pipeline (Jetson) this must be called AFTER
 * VIDIOC_STREAMON — the driver resets controls when streaming starts.
 * Always call set_vertical_blanking() first to widen the exposure window.
 *
 * Control ID: 0x00980911 (V4L2_CID_EXPOSURE / User Controls)
 * Driver range: min=1, max=65523
 */
static void set_exposure(int exposure) {
  set_ctrl_verified(V4L2_CID_EXPOSURE, exposure, "exposure");
}

static void init_mmap(void) {
  reqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = V4L2_MEMORY_MMAP;
  reqbuf.count  = 5;
  if (-1 == xioctl(fd, VIDIOC_REQBUFS, &reqbuf)) {
    perror("VIDIOC_REQBUFS");
    exit(errno);
  }

  if (reqbuf.count < 2) {
    printf("Not enough buffer memory\n");
    exit(EXIT_FAILURE);
  }

  //buffers = calloc(reqbuf.count, sizeof(*buffers));
  buffers = (decltype(buffers))calloc(reqbuf.count, sizeof(*buffers));

  assert(buffers != NULL);

  num_buffers = reqbuf.count;

  // Create the buffer memory maps
  struct v4l2_buffer buffer;
  for (unsigned int i = 0; i < reqbuf.count; i++) {
    memset(&buffer, 0, sizeof(buffer));
    buffer.type   = reqbuf.type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index  = i;

    // Note: VIDIOC_QUERYBUF, not VIDIOC_QBUF, is used here!
    if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buffer)) {
      perror("VIDIOC_QUERYBUF");
      exit(errno);
    }

    buffers[i].length = buffer.length;
    buffers[i].start  = mmap(
      NULL,
      buffer.length,
      PROT_READ | PROT_WRITE,
      MAP_SHARED,
      fd,
      buffer.m.offset
    );

    if (MAP_FAILED == buffers[i].start) {
      perror("mmap");
      exit(errno);
    }
  }
}

static void init_device() {
  struct v4l2_fmtdesc fmtdesc = {0};          // Used to enumerate/describe the supported formats
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  // Get the format with the largest index and use it
  // Repeatedly asking the driver
  // "Tell me about format #0"
  // "Tell me about format #1"
  // "Tell me about format #2"
  // xioctl() : ioctl() with retry logic
  while (0 == xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
    fmtdesc.index++;
  }

  printf("\nUsing format: %s\n", fmtdesc.description);

  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  fmt.fmt.pix.width       = SENSOR_WIDTH;
  fmt.fmt.pix.height      = SENSOR_HEIGHT;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;

  //fmt.fmt.pix.pixelformat = fmtdesc.pixelformat;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt)) {
    perror("VIDIOC_S_FMT");
    exit(errno);
  }

  // --------------------- Setting camera settings ---------------------
  // NOTE: Exposure is intentionally NOT set here on Jetson.
  // The Tegra VI/ISP pipeline resets sensor controls when VIDIOC_STREAMON
  // is called, so any value written here will be overwritten.
  // Exposure is applied in start_capturing() after VIDIOC_STREAMON instead.
  // See set_exposure() and set_vertical_blanking() for details.

  char format_code[5] = {0};
  memcpy(format_code, &fmt.fmt.pix.pixelformat, 4);

  printf(
    "Set format:\n"
    " Width: %d\n"
    " Height: %d\n"
    " Pixel format: %s\n"
    " Field: %d\n\n",
    fmt.fmt.pix.width,
    fmt.fmt.pix.height,
    format_code,
    fmt.fmt.pix.field
  );

  init_mmap();
}

static void start_capturing(void) {
  enum v4l2_buf_type type;

  struct v4l2_buffer buffer;
  for (unsigned int i = 0; i < num_buffers; i++) {
    /* Note that we set bytesused = 0, which will set it to the buffer length
     * See
     * - https://www.linuxtv.org/downloads/v4l-dvb-apis-new/uapi/v4l/vidioc-qbuf.html?highlight=vidioc_qbuf#description
     * - https://www.linuxtv.org/downloads/v4l-dvb-apis-new/uapi/v4l/buffer.html#c.v4l2_buffer
     */
    memset(&buffer, 0, sizeof(buffer));
    buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index  = i;

    // Enqueue the buffer with VIDIOC_QBUF
    if (-1 == xioctl(fd, VIDIOC_QBUF, &buffer)) {
      perror("VIDIOC_QBUF");
      exit(errno);
    }
  }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (-1 == xioctl(fd, VIDIOC_STREAMON, &type)) {
    perror("VIDIOC_STREAMON");
    exit(errno);
  }

  // --------------------- Apply exposure AFTER STREAMON ---------------------
  // On the Jetson Tegra VI/ISP pipeline VIDIOC_STREAMON reconfigures the
  // sensor, resetting any controls written before streaming began.
  //
  // Correct order:
  //   1. set_vertical_blanking() — widens the max exposure window
  //   2. set_exposure()          — sets exposure within that window
  //
  // vertical_blanking range : min=174, max=16399  (0x009e0901)
  // exposure range          : min=1,   max=65523  (0x00980911)
  set_vertical_blanking(DESIRED_VBLANKING);
  set_exposure(DESIRED_EXPOSURE);
}

static void stop_capturing(void) {
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type)) {
    perror("VIDIOC_STREAMOFF");
    exit(errno);
  }
}

/**
 * Draws a dot on the screen.
 *
 * Normally, the buffer would be processed here.
 */
static void process_image(const void *pBuffer) {
  //static int image_width  = 1600;
  //static int image_height = 1300;

  cv::Mat image(
    SENSOR_WIDTH,
    SENSOR_HEIGHT,
    CV_8UC1,
    (void *)pBuffer
  );

  cv::imshow("OV2311", image);
  cv::waitKey(1);

  //fputc('.', stdout);
  //fflush(stdout);
}

/**
 * Readout a frame from the buffers.
 */
static int read_frame(void) {
  struct v4l2_buffer buffer;
  memset(&buffer, 0, sizeof(buffer));
  buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;

  // Dequeue a buffer
  if (-1 == xioctl(fd, VIDIOC_DQBUF, &buffer)) {
    switch (errno) {
    case EAGAIN:
      // No buffer in the outgoing queue
      return 0;
    case EIO:
      // fall through
    default:
      perror("VIDIOC_DQBUF");
      exit(errno);
    }
  }

  assert(buffer.index < num_buffers);

  // Poll the live exposure value periodically to detect silent driver resets.
  // The Tegra ISP can override controls mid-stream (e.g. auto-exposure loop).
  // If you see the value drift from DESIRED_EXPOSURE here, the ISP AE is active.
  static int frame_num = 0;
  if (frame_num++ % 30 == 0) {
    struct v4l2_control readback = {};
    readback.id = V4L2_CID_EXPOSURE;
    if (0 == xioctl(fd, VIDIOC_G_CTRL, &readback)) {
      std::cout << "Exposure [frame " << frame_num << "] = "
                << readback.value << std::endl;
    }
  }

  process_image(buffers[buffer.index].start);

  // Enqueue the buffer again
  if (-1 == xioctl(fd, VIDIOC_QBUF, &buffer)) {
    perror("VIDIOC_QBUF");
    exit(errno);
  }

  return 1;
}

/**
 * Poll the device until it is ready for reading.
 *
 * See https://www.gnu.org/software/libc/manual/html_node/Waiting-for-I_002fO.html
 */
static void main_loop(void) {
  //unsigned int count = 100; // Record 100 frames
  //while(count-- > 0) {
  while (true) {
    fd_set fds;
    struct timeval tv;
    int r;
    for (;;) {
      // Clear the set of file descriptors to monitor, then add the fd for our device
      FD_ZERO(&fds);
      FD_SET(fd, &fds);

      // Set the timeout
      tv.tv_sec  = 2;
      tv.tv_usec = 0;

      /**
       * Arguments are
       * - number of file descriptors
       * - set of read fds
       * - set of write fds
       * - set of except fds
       * - timeval struct
       *
       * According to the man page for select, "nfds should be set to the highest-numbered file
       * descriptor in any of the three sets, plus 1."
       */
      r = select(fd + 1, &fds, NULL, NULL, &tv);

      if (-1 == r) {
        if (EINTR == errno)
          continue;

        perror("select");
        exit(errno);
      }

      if (0 == r) {
        fprintf(stderr, "select timeout\n");
        exit(EXIT_FAILURE);
      }

      if (read_frame())
        // Go to next iterartion of fhe while loop; 0 means no frame is ready in the outgoing queue.
        break;
    }
  }
}

int main(void) {
  // Open the device file
  fd = open(DEVICE, O_RDWR);
  if (fd < 0) {
    perror(DEVICE);
    return errno;
  }

  init_device();

  start_capturing();

  main_loop();

  stop_capturing();

  // Cleanup
  for (unsigned int i = 0; i < reqbuf.count; i++)
    munmap(buffers[i].start, buffers[i].length);
  free(buffers);
  close(fd);

  printf("\n\nDone.\n");
  return 0;
}
