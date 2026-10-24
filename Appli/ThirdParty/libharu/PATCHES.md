libHaru 2.4.6
Upstream commit: 3467749fd1c0ab6ca6ed424d053b1ea53c1bf67c

Local changes:
- UTF-8 Identity-H ToUnicode stream is emitted as a Unicode bfrange, not a CID range.
- CID map range batching closes exact 100-entry batches correctly.
- The 128 KiB CID-to-GID mapping table is allocated from the document memory manager rather than the thread stack.
- UTF-8 encoder scratch storage and its callers use the libHaru document allocator. CCITT image scratch allocations use the same allocator so the vendor tree contains no direct malloc/free calls.
- The firmware build disables PDF encryption. This read-only verification report does not require encryption; the upstream password API returns an unsupported-operation error in this target build instead of relying on an unavailable wall-clock syscall for encryption IDs.
- The report does not create image objects; image code remains in the upstream source but is discarded by section garbage collection when unreferenced.

Upstream license text is preserved in LICENSE. The upstream source is otherwise kept intact; changes are visible as a diff from commit above.
