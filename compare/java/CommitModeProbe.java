import java.io.InputStream;
import java.lang.reflect.Constructor;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;
import java.util.Properties;

/**
 * Prints the commit mode QuestDB really parsed from a root's conf/server.conf.
 *
 * SHOW PARAMETERS echoes the conf's text, so `cairo.commit.mode=not_a_mode` comes back as
 * typed while the server runs nosync. This runs the server's own configuration parser, from
 * the same jar, on the same file and environment, and prints what getCommitMode() returns. It
 * uses reflection
 * alone, so it compiles without QuestDB on the classpath. It takes the root as its argument,
 * and that root must have been started once, because the constructor reads conf/mime.types,
 * which the server extracts on its first start.
 */
public final class CommitModeProbe {
    private CommitModeProbe() {
    }

    public static void main(String[] arguments) throws Exception {
        String root = arguments[0];
        Properties properties = new Properties();
        try (InputStream conf = Files.newInputStream(Path.of(root, "conf", "server.conf"))) {
            properties.load(conf);
        }
        Class<?> configurationClass = Class.forName("io.questdb.PropServerConfiguration");
        Class<?> logClass = Class.forName("io.questdb.log.Log");
        Class<?> buildClass = Class.forName("io.questdb.BuildInformation");
        Class<?> holderClass = Class.forName("io.questdb.BuildInformationHolder");
        Class<?> logFactory = Class.forName("io.questdb.log.LogFactory");

        Object log = logFactory.getMethod("getLog", String.class).invoke(null, "probe");
        Object build = holderClass.getConstructor().newInstance();
        Constructor<?> constructor = configurationClass.getConstructor(
                String.class, Properties.class, Map.class, logClass, buildClass);
        Object configuration = constructor.newInstance(
                root, properties, System.getenv(), log, build);

        Object cairo = configurationClass.getMethod("getCairoConfiguration").invoke(configuration);
        Class<?> cairoClass = Class.forName("io.questdb.cairo.CairoConfiguration");
        int mode = (Integer) cairoClass.getMethod("getCommitMode").invoke(cairo);
        Class<?> modes = Class.forName("io.questdb.cairo.CommitMode");
        String name = "unknown";
        for (String candidate : new String[] {"ASYNC", "SYNC", "NOSYNC"}) {
            if (mode == modes.getField(candidate).getInt(null)) {
                name = candidate.toLowerCase(java.util.Locale.ROOT);
            }
        }
        System.out.println("commit_mode=" + name + " value=" + mode);
    }
}
