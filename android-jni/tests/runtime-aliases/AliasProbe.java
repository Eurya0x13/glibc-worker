import dev.glibc.worker.GuestLibraries;
import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

final class AliasProbe {
  private static File prepare(File store, File installed, String catalog) throws IOException {
    return GuestLibraries.prepare(
        store, installed, new ByteArrayInputStream(catalog.getBytes(StandardCharsets.US_ASCII)));
  }

  private static void expectRejected(File store, File installed, String catalog)
      throws IOException {
    try {
      prepare(store, installed, catalog);
    } catch (IOException expected) {
      return;
    }
    throw new AssertionError("Invalid glibc alias configuration was accepted");
  }

  public static void main(String[] args) throws IOException {
    File store = new File(args[0]);
    File installed = new File(args[1]);
    String catalog = new String(Files.readAllBytes(new File(installed, "glibc-libs.tsv").toPath()),
        StandardCharsets.US_ASCII);
    File directory = prepare(store, installed, catalog);
    if (!prepare(store, installed, catalog).equals(directory)) {
      throw new AssertionError("Staging did not reuse the alias directory");
    }

    expectRejected(store, installed, catalog + catalog);
    String badHash = "0000000000000000000000000000000000000000000000000000000000000000";
    expectRejected(store, installed, catalog.replaceFirst("[0-9a-f]{64}", badHash));

    Path link = new File(directory, "libdl.so.2").toPath();
    Path target = Files.readSymbolicLink(link);
    Files.delete(link);
    Files.createSymbolicLink(link, link.getFileName());
    expectRejected(store, installed, catalog);
    Files.delete(link);
    Files.createSymbolicLink(link, target);
    System.out.println(directory.getPath());
  }
}
