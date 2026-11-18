import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordedEvent;
import jdk.jfr.consumer.RecordedThread;
import jdk.jfr.consumer.RecordingFile;

public final class SdkVmSignals {
  private static final String NATIVE_SAMPLE = "jdk.NativeMethodSample";
  private static final String JAVA_SAMPLE = "jdk.ExecutionSample";
  private static final String GUEST_THREAD = "sdk-guest-sample";
  private static final int STAGE = 0;
  private static final int COMMAND = 4;
  private static final int NATIVE_READY = 1;
  private static final int JAVA_READY = 2;
  private static final int JAVA_RETURNED = 3;
  private static final int STACKS_RELEASED = 4;
  private static final VarHandle INTS =
      MethodHandles.byteBufferViewVarHandle(int[].class, ByteOrder.nativeOrder());

  private static void requireStage(ByteBuffer control, int stage) throws InterruptedException {
    long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
    while ((int) INTS.getAcquire(control, STAGE) < stage) {
      if (System.nanoTime() >= deadline) {
        throw new AssertionError("Guest did not reach stage " + stage);
      }
      Thread.sleep(1);
    }
  }

  static void sampleWhileGuestParked(ByteBuffer control) {
    INTS.setRelease(control, STAGE, JAVA_READY);
    while ((int) INTS.getAcquire(control, COMMAND) < JAVA_RETURNED) {
      Thread.onSpinWait();
    }
  }

  @SuppressWarnings("removal")
  private static void checkSuspended(Thread target, ByteBuffer control, CountDownLatch returned)
      throws InterruptedException {
    target.suspend();
    try {
      INTS.setRelease(control, COMMAND, STACKS_RELEASED);
      requireStage(control, STACKS_RELEASED);
      if (returned.await(200, TimeUnit.MILLISECONDS)) {
        throw new AssertionError("Suspended thread crossed the JNI return");
      }
    } finally {
      target.resume();
    }
  }

  private static boolean hasFrame(
      RecordedEvent event, long javaThreadId, String className, String methodName) {
    RecordedThread thread = event.getThread("sampledThread");
    return thread != null && thread.getJavaThreadId() == javaThreadId
        && GUEST_THREAD.equals(thread.getJavaName()) && event.getStackTrace() != null
        && event.getStackTrace().getFrames().stream().anyMatch(frame
            -> frame.getMethod().getType().getName().equals(className)
                && frame.getMethod().getName().equals(methodName));
  }

  private static boolean inWindow(RecordedEvent event, Instant start, Instant end) {
    Instant time = event.getStartTime();
    return !time.isBefore(start) && time.isBefore(end);
  }

  static int[] run(ByteBuffer control, long address, String directory) throws Exception {
    CountDownLatch returned = new CountDownLatch(1);
    AtomicInteger result = new AtomicInteger();
    Thread target = new Thread(() -> {
      result.set(SdkProbe.runVmSignalGuest(address, control));
      returned.countDown();
    }, GUEST_THREAD);
    Path path = Files.createTempFile(Path.of(directory), "sdk-vm-signals-", ".jfr");
    try (Recording recording = new Recording()) {
      recording.enable(NATIVE_SAMPLE).withPeriod(Duration.ofMillis(10)).withStackTrace();
      recording.enable(JAVA_SAMPLE).withPeriod(Duration.ofMillis(10)).withStackTrace();
      recording.start();
      target.start();
      Instant nativeStart, nativeEnd, javaStart, javaEnd;
      try {
        requireStage(control, NATIVE_READY);
        nativeStart = Instant.now();
        Thread.sleep(600);
        nativeEnd = Instant.now();

        INTS.setRelease(control, COMMAND, JAVA_READY);
        requireStage(control, JAVA_READY);
        javaStart = Instant.now();
        Thread.sleep(600);
        javaEnd = Instant.now();

        INTS.setRelease(control, COMMAND, JAVA_RETURNED);
        requireStage(control, JAVA_RETURNED);
        checkSuspended(target, control, returned);
      } finally {
        INTS.setRelease(control, COMMAND, STACKS_RELEASED);
        target.join(5000);
        if (target.isAlive() || result.get() != 73) {
          throw new AssertionError("Guest did not return after VM resume");
        }
      }
      recording.stop();
      recording.dump(path);
      try (RecordingFile file = new RecordingFile(path)) {
        int nativeSamples = 0;
        int javaSamples = 0;
        while (file.hasMoreEvents()) {
          RecordedEvent event = file.readEvent();
          String name = event.getEventType().getName();
          if (name.equals(NATIVE_SAMPLE) && inWindow(event, nativeStart, nativeEnd)
              && hasFrame(event, target.getId(), "SdkProbe", "runVmSignalGuest")) {
            ++nativeSamples;
          }
          if (name.equals(JAVA_SAMPLE) && inWindow(event, javaStart, javaEnd)
              && hasFrame(event, target.getId(), "SdkVmSignals", "sampleWhileGuestParked")) {
            ++javaSamples;
          }
        }
        return new int[] {nativeSamples, javaSamples};
      }
    } finally {
      Files.delete(path);
    }
  }
}
