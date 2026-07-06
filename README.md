# vfio-pci-extras

Out-of-tree VFIO PCI variant drivers for live migration testing from
user space (QEMU) in nested emulated environments.

## igb-vfio-pci

Variant driver for the Intel 82576 Virtual Function (PCI device
10ca). It extends the vfio-pci-core framework and implements the VFIO
migration protocol v2 with stop-copy and pre-copy support.

Device state is saved and restored through a migration BAR - a 64 KB
MMIO region on VF BAR2, exposed by the emulated PF when
`x-vf-migration=on` and hidden from the guest. The BAR carries VF
state as an opaque blob transferred via DMA - the driver allocates a
buffer, registers its DMA address with the PF device, and triggers a
transfer. The driver discovers the BAR through a vendor-specific PCI
capability (magic `0x4D494742` / "MIGB").

Dirty page tracking uses a shared buffer protocol.  At enable time the
driver allocates a buffer (header + bitmap) and registers its DMA
address with the PF device. For each query the driver writes (IOVA, size,
page_size) into the buffer, issues a single MMIO doorbell
(DIRTY_CTRL=QUERY), and the device writes the dirty bitmap and
completion status back into the same buffer. The buffer is dynamically
sized to cover the largest tracked range.

Migration state machine:

```
Saving  (source): RUNNING -> PRE_COPY -> STOP_COPY
Resuming (target): RESUMING -> STOP -> RUNNING
Abort  (source): PRE_COPY -> RUNNING
```

## Migration BAR layout

The migration BAR is a 64 KB MMIO region on VF BAR2, hidden from the
guest and only visible to this driver. It contains a register header
for control and DMA-based state/dirty data transfer.

### Discovery

The BAR is discovered through a vendor-specific PCI capability
(`PCI_CAP_ID_VNDR`, id 0x09). The driver walks the capability list
and matches on the magic value.

Capability layout (16 bytes):

| Offset | Size | Name    | Description                                  |
|--------|------|---------|----------------------------------------------|
| 0x00   | 1    | cap_id  | `PCI_CAP_ID_VNDR` (0x09)                     |
| 0x01   | 1    | next    | Next capability pointer                      |
| 0x02   | 1    | cap_len | Capability length (16)                       |
| 0x03   | 1    | version | `IGB_MIG_CAP_VERSION` (1)                    |
| 0x04   | 4    | magic   | `0x4D494742` ("MIGB", little-endian)         |
| 0x08   | 4    | bar_id  | BAR index for the migration region (2)       |
| 0x0C   | 4    | flags   | Feature flags: bit 0 = STATE, bit 1 = DIRTY  |

After matching the magic, the driver reads `bar_id` to identify which
BAR to map, then validates `VERSION` and `CAPS`.

### Register header (0x000 – 0x0FF)

| Offset | Name                | R/W | Description                                          |
|--------|---------------------|-----|------------------------------------------------------|
| 0x000  | DEVICE_STATE        | RW  | Migration state machine control                      |
| 0x004  | STATUS              | RO  | Flags[2:0]: DATA_AVAIL, ERROR, QUIESCED; Error code[15:8] |
| 0x008  | CAPS                | RO  | F_STATE, F_DIRTY, max_ranges[11:8], pgsizes[31:12]   |
| 0x00C  | VERSION             | RO  | Interface version (1)                                |
| 0x010  | DATA_SIZE           | RW  | Max state size at reset, actual after save            |
| 0x014  | DATA_XFER           | WO  | Trigger DMA save or DMA load                         |
| 0x018  | DATA_BUF_ADDR_LO    | WO  | Low 32 bits of state DMA buffer address              |
| 0x01C  | DATA_BUF_ADDR_HI    | WO  | High 32 bits of state DMA buffer address             |
| 0x020  | DIRTY_PGSIZE        | RW  | Dirty tracking page granularity                      |
| 0x024  | DIRTY_CTRL          | WO  | Dirty control: 0=DISABLE, 1=ENABLE, 2=QUERY          |
| 0x028  | DIRTY_RANGE_IOVA_LO | WO  | Low 32 bits of tracked range start (enable-time)     |
| 0x02C  | DIRTY_RANGE_IOVA_HI | WO  | High 32 bits of tracked range start (enable-time)    |
| 0x030  | DIRTY_RANGE_SIZE_LO | WO  | Low 32 bits of tracked range size (enable-time)      |
| 0x034  | DIRTY_RANGE_SIZE_HI | WO  | High 32 bits of tracked range size (enable-time)     |
| 0x038  | DIRTY_BUF_ADDR_LO   | WO  | Low 32 bits of shared buffer address                 |
| 0x03C  | DIRTY_BUF_ADDR_HI   | WO  | High 32 bits of shared buffer address                |
| 0x040  | DIRTY_STATUS        | RO  | Result of last DIRTY_CTRL (0=OK, 1-6=error)          |

### State data transfer (DMA)

State data is transferred via a driver-provided DMA buffer. The driver
allocates a buffer, writes its PF DMA address to
`DATA_BUF_ADDR_LO/HI`, and writes `DATA_XFER` to trigger the transfer.

On the source, after the device enters STOP_COPY (or PRE_COPY), it
serializes VF state internally and sets `DATA_AVAIL` in `STATUS`.
The driver reads `DATA_SIZE` to learn how many bytes were produced,
then writes `DATA_XFER` - the device DMA-writes the state blob into
the driver's buffer. On the destination, the driver fills its buffer
with the received blob, writes `DATA_SIZE`, and writes `DATA_XFER` -
the device DMA-reads the blob and restores VF state.

The state blob contains a versioned header followed by per-VF
register (offset, value) pairs covering control, interrupt, RX/TX
queue, mailbox, statistics, EITR, receive addresses (RA/RA2), and
per-VF config (VMOLR, VMVIR, PSRTYPE).  It also includes TX context
descriptors, interrupt routing (VTIVAR), and VFRE/VFTE enable bits.

### Dirty page tracking

**Enable**: the driver programs one or more tracked ranges by writing
`DIRTY_RANGE_IOVA_LO/HI` + `DIRTY_RANGE_SIZE` + `DIRTY_CTRL=ENABLE`
for each range.  After each ENABLE the driver reads `DIRTY_STATUS` to
check for errors.  If any range fails, the driver sends
`DIRTY_CTRL=DISABLE` to tear down all ranges and reports failure.

The device maintains one dirty tracking engine per range, each with
its own bitmap scoped to the range boundaries.  The CAPS register
advertises the maximum number of ranges the device supports
(`max_ranges` in bits [11:8]).  If the VFIO core requests more
ranges than the device supports, the driver merges them with
`vfio_combine_iova_ranges()`.

`DIRTY_STATUS` values:

| Value | Name             | Description                        |
|-------|------------------|------------------------------------|
| 0     | OK               | Success                            |
| 1     | TOO_MANY_RANGES  | Exceeds max_ranges from CAPS       |
| 2     | BAD_RANGE        | Invalid range (zero size, out of bounds, misaligned) |
| 3     | BAD_PGSIZE       | Invalid or misaligned page size    |
| 4     | NOT_ENABLED      | Query without prior enable         |
| 5     | NO_BUFFER        | Query without shared buffer        |
| 6     | DMA_FAILED       | DMA write of dirty bitmap failed   |

The driver also allocates a shared buffer sized to the header
plus a bitmap covering the largest tracked range, and registers its
PF DMA address via `DIRTY_BUF_ADDR_LO/HI`.

**Query**: for each `log_read_and_clear` call, the driver writes the
query parameters (IOVA, size, page_size) into the shared buffer,
issues a single MMIO doorbell (`DIRTY_CTRL=QUERY`), and polls the
buffer's `status` field for completion.  The device validates the
requested range against enabled ranges (IOVA, size, page alignment),
writes the dirty bitmap into the buffer, clears the tracked bits
only after successful DMA, and sets `status = COMPLETE`.  The driver
then reads `DIRTY_STATUS` to check for errors.  Each bit in the
bitmap represents one page at `DIRTY_PGSIZE` granularity.

The shared buffer is cache-line aligned (64 bytes) to separate
driver-polled and device-written fields:

```
Offset  Field              Written by  Description
0x00    iova               driver      Query range start
0x08    size               driver      Query range size
0x10    page_size          driver      Page granularity
0x14    flags              driver      Reserved (must be 0)
0x18    reserved[10]       -           Pad to 64-byte cache line
0x40    status             device      0 = pending, 1 = complete
0x44    bitmap_size        device      Bytes written to bitmap
0x48    dirty_page_count   device      Number of set bits in bitmap
0x4C    reserved           -           Padding
0x50    dma_count          device      Total DMA writes since enable
0x58    reserved[10]       -           Pad to 64-byte cache line
0x80    bitmap[]           device      Dirty page bitmap
```

**Disable**: the driver writes `DIRTY_CTRL=DISABLE`, clears
`DIRTY_BUF_ADDR`, and frees the shared buffer.

### Device states

| Value | Name      | Description                                   |
|-------|-----------|-----------------------------------------------|
| 0     | ERROR     | Unrecoverable error                           |
| 1     | STOP      | Device quiesced, no DMA                       |
| 2     | RUNNING   | Normal operation                              |
| 3     | STOP_COPY | Stopped, state available for DMA save         |
| 4     | RESUMING  | Accepting state via DMA load                  |
| 5     | PRE_COPY  | Running with dirty tracking + state snapshots |

### STATUS error codes

When the `ERROR` bit (bit 1) is set in `STATUS`, bits [15:8] contain
an error code identifying the failure:

| Value | Name        | Description                              |
|-------|-------------|------------------------------------------|
| 0     | (none)      | No error                                 |
| 1     | BAD_MAGIC   | State blob magic mismatch                |
| 2     | BAD_VERSION | State blob version mismatch              |
| 3     | BAD_SIZE    | State blob too large or empty            |
| 4     | BAD_VFN     | VF number mismatch (source != destination) |
| 5     | DMA_FAILED  | DMA transfer to/from state buffer failed |
| 6     | NO_BUFFER   | DATA_XFER without buffer address set     |

## Build

```sh
make KDIR=/path/to/linux
sudo make modules_install KDIR=/path/to/linux
```

Defaults to the running kernel if `KDIR` is not set. The `Kbuild`
file auto-detects missing symbols (`vfio_check_precopy_ioctl`,
`kzalloc_obj`) for compatibility with older kernels.

## Credits

Alex Williamson suggested the overall approach: a hidden migration BAR
discovered via a vendor-specific PCI capability, the "vf-migration"
device property to gate the feature. Thanks for the ever ongoing
support and valuable discussions throughout these years.

## AI disclaimer

Claude was used to analyze the IGB PF and VF internal state and
identify the pain points of a working live migration of such devices.
The generated code served as a starting point but *significant* time
was then spent cleaning up, reworking, and shaping it into a clear,
reviewable proposal.

## License

GPL-2.0-only
