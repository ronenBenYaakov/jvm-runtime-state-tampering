import java.lang.management.ManagementFactory;
import java.lang.ref.PhantomReference;
import java.lang.ref.ReferenceQueue;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

public class Main {

    private static final Map<String, Object> LIVE_MAP = new ConcurrentHashMap<>();
    private static final ReferenceQueue<Object> REFERENCE_QUEUE = new ReferenceQueue<>();
    private static final Map<PhantomRefKey, String> TRACKED_PROBES = new ConcurrentHashMap<>();

    private static final AtomicInteger PROCESSED_SURVIVORS = new AtomicInteger(0);
    private static volatile boolean running = true;

    static class PhantomRefKey extends PhantomReference<Object> {
        final String key;

        public PhantomRefKey(String key, Object referent, ReferenceQueue<Object> q) {
            super(referent, q);
            this.key = key;
        }
    }

    public static void main(String[] args) throws InterruptedException {
        String pid = ManagementFactory.getRuntimeMXBean().getName().split("@")[0];
        System.out.println("Java process started.");
        System.out.println("PID: " + pid);
        System.out.println("Workers started.\nWaiting...");

        // 1. Cleaner Thread: Consumes enqueued phantom references
        Thread cleanerThread = new Thread(() -> {
            while (running) {
                try {
                    PhantomRefKey ref = (PhantomRefKey) REFERENCE_QUEUE.remove(50);
                    if (ref != null) {
                        TRACKED_PROBES.remove(ref);
                        PROCESSED_SURVIVORS.incrementAndGet();
                    }
                } catch (InterruptedException e) {
                    break;
                }
            }
        }, "Cleaner-Thread");
        cleanerThread.setDaemon(true);
        cleanerThread.start();

        // 2. Producer Thread: Creates and deliberately drops references
        Thread producerThread = new Thread(() -> {
            long counter = 0;
            while (running) {
                try {
                    String id = "entry-" + (++counter);

                    // Allocate 64 KB memory chunk
                    Object payload = new byte[64 * 1024];

                    // Attach phantom reference probe
                    PhantomRefKey probe = new PhantomRefKey(id, payload, REFERENCE_QUEUE);
                    TRACKED_PROBES.put(probe, id);

                    // Drop 4 out of every 5 objects immediately to create unreachables
                    if (counter % 5 != 0) {
                        payload = null; // Eligible for collection
                    } else {
                        LIVE_MAP.put(id, payload); // Kept alive in map
                    }

                    // Request GC check every 20 allocations to process pending queues
                    if (counter % 20 == 0) {
                        System.gc();
                    }

                    Thread.sleep(20);
                } catch (InterruptedException e) {
                    break;
                }
            }
        }, "Producer-Thread");
        producerThread.setDaemon(true);
        producerThread.start();

        // 3. Monitor Loop: Reports live metrics
        while (running) {
            Thread.sleep(1000);
            System.out.printf("Live map entries: %d | survivors: %d%n",
                    LIVE_MAP.size(), PROCESSED_SURVIVORS.get());
        }
    }
}