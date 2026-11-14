package conformance;

/** Negative JNI cases in the same child ClassLoader as ResourceCapacity. */
public final class FailureCases {
  private static void require(boolean condition, String message) {
    if (!condition)
      throw new AssertionError(message);
  }

  public static int verify(int available) throws Exception {
    boolean overflow = false;
    try {
      ResourceCapacity.recurse(0, 32);
    } catch (StackOverflowError expected) {
      overflow = true;
    }
    require(overflow, "Guest depth failure must reach Java as StackOverflowError");
    require(ResourceCapacity.recurse(1, 32) == 12345,
        "Guest must query and clear exceptions at the depth limit");
    boolean rejected = false;
    try {
      ResourceCapacity.failureProbe(2, 0);
    } catch (IllegalArgumentException expected) {
      rejected = true;
    }
    require(rejected, "Oversized registration must have an explicit exception");
    require(ResourceCapacity.failureProbe(3, 0) == -1,
        "Failed JNI status must be JNI_ERR, never JNI_OK");

    int contexts = ResourceCapacity.failureProbe(8, 0);
    require(contexts >= 2 && contexts <= 256, "Context configuration");
    require(available > 0 && available < contexts, "Available context configuration");
    Thread[] threads = new Thread[available];
    Throwable[] errors = new Throwable[available + 1];
    require(ResourceCapacity.failureProbe(6, 0) == 0, "Reset context barrier");
    try {
      for (int i = 0; i < threads.length; i++) {
        final int index = i;
        threads[i] = new Thread(() -> {
          try {
            require(ResourceCapacity.failureProbe(4, 0) == 0, "Held worker timeout");
          } catch (Throwable error) {
            errors[index] = error;
          }
        }, "audit-context-" + i);
        threads[i].start();
      }
      long deadline = System.nanoTime() + 10_000_000_000L;
      while (ResourceCapacity.failureProbe(7, 0) != threads.length && System.nanoTime() < deadline)
        Thread.sleep(2);
      require(ResourceCapacity.failureProbe(7, 0) == threads.length, "Fill all contexts");
      Thread extra = new Thread(() -> {
        try {
          ResourceCapacity.failureProbe(9, 0);
          errors[available] = new AssertionError("Exhaustion fabricated a successful call");
        } catch (OutOfMemoryError expected) {
          // A new thread owns no bridge context; the Host must throw directly.
        } catch (Throwable error) {
          errors[available] = error;
        }
      }, "audit-context-overflow");
      extra.start();
      extra.join(5000);
      require(!extra.isAlive(), "Context failure must not hang");
    } finally {
      ResourceCapacity.failureProbe(5, 0);
      for (Thread thread : threads)
        if (thread != null)
          thread.join(5000);
    }
    for (int i = 0; i < threads.length; i++) require(!threads[i].isAlive(), "Worker release");
    for (Throwable error : errors)
      if (error != null)
        throw new AssertionError(error);
    return 5;
  }

  public static boolean recovery() throws Exception {
    final Throwable[] error = new Throwable[1];
    Thread thread = new Thread(() -> {
      try {
        require(ResourceCapacity.failureProbe(9, 0) == 12345, "Context reuse");
      } catch (Throwable failure) {
        error[0] = failure;
      }
    });
    thread.start();
    thread.join(5000);
    require(!thread.isAlive(), "Context recovery timeout");
    if (error[0] != null)
      throw new AssertionError(error[0]);
    return true;
  }
}
