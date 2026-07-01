// spike/SpikeClient.java — throwaway Java client for the network+GPU round-trip spike.
// Speaks the minimal wire protocol implemented by spike_server.cpp: length-prefixed frames,
// a zero-length HEALTH-equivalent request, and a 5-byte SCAN request (opcode=1, u32 threshold).
// NOT production code — no Spring Boot, no dependencies, just java.net.Socket.
//
// Usage: java SpikeClient <host> <port> <iterations>
// Prints CSV to stdout: kind,iteration,round_trip_ns,server_seconds,count
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class SpikeClient {
    private static final int WARMUP = 5;
    private static final int SCAN_THRESHOLD = 8_388_608; // half of MATRIX_SCAN_COLUMN_SIZE (16,777,216)

    public static void main(String[] args) throws IOException {
        final String host = args.length > 0 ? args[0] : "127.0.0.1";
        final int port = args.length > 1 ? Integer.parseInt(args[1]) : 7070;
        final int iterations = args.length > 2 ? Integer.parseInt(args[2]) : 50;

        try (Socket sock = new Socket(host, port)) {
            sock.setTcpNoDelay(true);
            final DataOutputStream out = new DataOutputStream(sock.getOutputStream());
            final DataInputStream in = new DataInputStream(sock.getInputStream());

            System.out.println("kind,iteration,round_trip_ns,server_seconds,count");

            for (int i = -WARMUP; i < iterations; i++) {
                final long t0 = System.nanoTime();
                writeFrame(out, new byte[0]);
                readFrame(in);
                final long t1 = System.nanoTime();
                if (i >= 0) System.out.println("health," + i + "," + (t1 - t0) + ",,");
            }

            final ByteBuffer req = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN);
            req.put(0, (byte) 1);
            req.putInt(1, SCAN_THRESHOLD);
            for (int i = -WARMUP; i < iterations; i++) {
                final long t0 = System.nanoTime();
                writeFrame(out, req.array());
                final byte[] resp = readFrame(in);
                final long t1 = System.nanoTime();
                if (i >= 0) {
                    final ByteBuffer rb = ByteBuffer.wrap(resp).order(ByteOrder.LITTLE_ENDIAN);
                    final double serverSeconds = Double.longBitsToDouble(rb.getLong(0));
                    final long count = rb.getLong(8);
                    System.out.println("scan," + i + "," + (t1 - t0) + "," + serverSeconds + "," + count);
                }
            }
        }
    }

    private static void writeFrame(DataOutputStream out, byte[] payload) throws IOException {
        final ByteBuffer lenBuf = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(0, payload.length);
        out.write(lenBuf.array());
        out.write(payload);
        out.flush();
    }

    private static byte[] readFrame(DataInputStream in) throws IOException {
        final byte[] lenBytes = new byte[4];
        in.readFully(lenBytes);
        final int len = ByteBuffer.wrap(lenBytes).order(ByteOrder.LITTLE_ENDIAN).getInt();
        final byte[] payload = new byte[len];
        if (len > 0) in.readFully(payload);
        return payload;
    }
}
