package compat;

public final class SignalReclaimSuite {
  private static native void prepare();

  private static native void receive();

  private static native void reclaim();

  public static void main(String[] args) throws InterruptedException {
    System.load(args[0]);
    prepare();

    Thread recipient = new Thread(SignalReclaimSuite::receive, "signal-recipient");
    recipient.start();
    reclaim();
    recipient.join();

    System.out.println("{\"result\":\"PASS\",\"case\":\"live-vm-signal-reclaim\"}");
  }
}
