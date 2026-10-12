# Why: Rationale for TOAST Compression Modules

## Motivation and Industry Context

Storage cost, memory bandwidth, and I/O throughput are primary bottlenecks in large PostgreSQL deployments. While storage hardware has become faster, database sizes continue to grow exponentially with JSON documents, event payloads, and analytical datums.

Standard PostgreSQL compression (`pglz` and `lz4`) is limited in scope:
1. **Generic LZ vs. Domain Structures**:
   Byte-oriented LZ algorithms search for repeating byte strings within a sliding window. They are blind to data structure:
   - In sparse integers, coordinates, or timestamps, small numbers consume 4 or 8 bytes with long runs of leading zeros, which LZ compresses inefficiently.
   - In micro-payloads (e.g. 100-byte to 500-byte JSON documents), sliding-window algorithms fail to find sufficient repetitive patterns inside a single datum to overcome their compression header overhead.
2. **Missing Dictionary Capabilities**:
   Algorithms like Facebook's Zstandard (`zstd`) achieve dramatically superior compression ratios when trained on a shared dictionary representing common schema keys and patterns. Without dictionary support, repetitive JSON keys waste vast amounts of storage.
3. **Inability to Combine Specialized Transformations**:
   Optimal data compression often requires a multi-stage transformation:
   - First, apply domain encoding (e.g., Avro ZigZag varint encoding to compress integers).
   - Second, pass the structured stream to an entropy encoder like Zstandard.
   Standard PostgreSQL provides no mechanism to pipeline transformations.

## How This Feature Solves the Problem

1. **`pg_avro` for Compact Structural Serialization**:
   Translates wide integer and structured datums into variable-length ZigZag bytes, reducing uncompressed data size by up to 50% before block compression even begins.

2. **`pg_zstd` for Industry-Leading Ratios**:
   Leverages Zstandard's advanced entropy coding (FSE/Huffman) and dictionary support. For repetitive JSON or semi-structured tables, `pg_zstd` frequently cuts on-disk TOAST size in half compared to `lz4` or `pglz`.

3. **`pg_lz4` for Ultra-Low CPU Overhead**:
   Provides blazing-fast compression and decompression speeds (hundreds of MB/s per core) when CPU time is the critical resource.

4. **`pg_cascade` for Composable Synergies**:
   Allows database designers to assemble multi-stage pipelines tailored to their workload:
   $$\text{Wide Structured Input} \longrightarrow \text{pg\_avro} \longrightarrow \text{pg\_zstd} \longrightarrow \text{Compact Storage}$$
   This composability achieves compression ratios previously unattainable in standard relational databases.
