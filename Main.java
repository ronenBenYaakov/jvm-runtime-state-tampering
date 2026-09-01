import java.io.*;
import java.lang.ref.Cleaner;
import java.net.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

public class Main {

    static final int PORT = 9000;
    static final Cleaner SERVER_CLEANER = Cleaner.create();

    static final AtomicLong totalRequests = new AtomicLong();
    static final AtomicLong cleanedClientBuffers = new AtomicLong();
    static final AtomicLong allocatedDirectMemoryMB = new AtomicLong();

    // Represents an active client session backed by high-speed Off-Heap Native Memory
    static class ClientSession {
        final long clientId;
        final ByteBuffer offHeapBuffer; // 1 MB off-heap native memory
        final Cleaner.Cleanable cleanable;

        public ClientSession(long clientId) {
            this.clientId = clientId;
            // 1. Allocate 1 MB off-heap native memory (uses PhantomReference under the hood)
            this.offHeapBuffer = ByteBuffer.allocateDirect(1024 * 1024);
            allocatedDirectMemoryMB.incrementAndGet();

            // 2. Register a Cleaner (PhantomReference) to track buffer cleanup when client is dropped
            this.cleanable = SERVER_CLEANER.register(this, new BufferCleanupTask(clientId));
        }

        private static class BufferCleanupTask implements Runnable {
            private final long id;

            BufferCleanupTask(long id) {
                this.id = id;
            }

            @Override
            public void run() {
                // Fired by JVM ReferenceQueue when ClientSession becomes unreachable
                cleanedClientBuffers.incrementAndGet();
                allocatedDirectMemoryMB.decrementAndGet();
            }
        }
    }

    static void handleClient(Socket socket) {
        long clientId = Thread.currentThread().threadId();

        System.out.println("[+] Client connected: " + socket.getRemoteSocketAddress());

        try (
                socket;
                BufferedReader reader = new BufferedReader(
                        new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
                BufferedWriter writer = new BufferedWriter(
                        new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8))
        ) {
            String line;
            while ((line = reader.readLine()) != null) {
                totalRequests.incrementAndGet();

                switch (line.trim().toUpperCase()) {
                    case "PING":
                        writer.write("PONG\n");
                        break;

                    case "WORK":
                        // Create 10 temporary off-heap client sessions
                        for (int i = 0; i < 10; i++) {
                            // Each session allocates 1 MB of native memory and attaches a PhantomReference
                            ClientSession session = new ClientSession(clientId);
                            session.offHeapBuffer.putInt(0, 42);
                            // `session` is intentionally dropped here (becomes eligible for GC)
                        }
                        // Request GC to trigger ReferenceQueue cleanups
                        System.gc();

                        writer.write("WORK COMPLETE (Allocated 10 MB off-heap)\n");
                        break;

                    case "STATUS":
                        writer.write(String.format(
                                "Requests: %d | Active Native Buffers Cleaned: %d | Direct RAM in use: ~%d MB%n",
                                totalRequests.get(),
                                cleanedClientBuffers.get(),
                                allocatedDirectMemoryMB.get()
                        ));
                        break;

                    case "QUIT":
                        writer.write("GOODBYE\n");
                        writer.flush();
                        return;

                    default:
                        writer.write("Commands: PING WORK STATUS QUIT\n");
                }
                writer.flush();
            }
        } catch (IOException e) {
            System.out.println("[-] Client " + clientId + " disconnected");
        }
    }

    static void statisticsLoop() {
        while (true) {
            try {
                Thread.sleep(2000);
            } catch (InterruptedException e) {
                return;
            }

            System.out.printf("[SERVER] Requests: %d | Cleaned Buffers: %d | Native Buffers Pending: %d%n",
                    totalRequests.get(),
                    cleanedClientBuffers.get(),
                    allocatedDirectMemoryMB.get());
        }
    }

    public static void main(String[] args) throws Exception {
        System.out.println("High-Performance NIO Server (Off-Heap Buffer Pool)");
        System.out.println("PID: " + ProcessHandle.current().pid());

        Thread stats = new Thread(Main::statisticsLoop, "stats");
        stats.setDaemon(true);
        stats.start();

        ExecutorService clients = Executors.newCachedThreadPool();

        try (ServerSocket server = new ServerSocket(PORT)) {
            System.out.println("Listening on TCP port " + PORT + "...\n");

            while (true) {
                Socket client = server.accept();
                clients.submit(() -> handleClient(client));
            }
        }
    }
}