# vfio-pci-extras

Out-of-tree VFIO PCI variant drivers for live migration testing from
user space (QEMU) in nested emulated environments.

## igb-vfio-pci

Live migration of VFIO-passthrough devices requires hardware with
migration support, which is scarce and hard to debug. An emulated
device provides a fully controlled testbed for developing and
validating the software stack -- variant driver, VFIO migration v2
framework, QEMU, libvirt.

The `igb-vfio-pci` driver is a VFIO PCI variant driver for Intel
82576 Virtual Functions (PCI device 0x10ca). It extends the
`vfio-pci-core` framework and implements the VFIO migration protocol
v2 with stop-copy and pre-copy support. The target scenario is nested
virtualization:

```
L0 QEMU
  igb PF with x-vf-migration=on
  └── VFs with migration DVSEC

L1 kernel
  igb-vfio-pci variant driver
  translates VFIO migration v2 ioctls → DVSEC config writes

L1 QEMU (stock, unmodified)
  vfio-pci device model, standard migration fd

L2 guest
  standard igbvf driver, unaware of migration
```

The migration interface is exposed through a DVSEC in the VF extended
config space(see `DVSEC register layout`_ below), advertised by the L0
QEMU emulated PF when `x-vf-migration=on`.

All control is via PCI config space writes; commands are synchronous.
Dirty page tracking is controlled through DVSEC commands and a shared
DMA buffer for enable, query, and bitmap transfer.

Migration state machine:

```
Saving   (source): RUNNING -> PRE_COPY -> STOP_COPY
Resuming (target): RESUMING -> STOP -> RUNNING
Abort    (source): PRE_COPY -> RUNNING
```

## DVSEC register layout

The DVSEC is at offset 0x160 in VF extended config space.

### DVSEC headers (standard PCIe)

| Offset | Size | Name           | Description                              |
|--------|------|----------------|------------------------------------------|
| 0x00   | 4    | Ext cap header | cap_id=0x23, ver=1, next                 |
| 0x04   | 4    | DVSEC header 1 | len[31:20], rev[19:16], vendor_id[15:0]  |
| 0x08   | 2    | DVSEC header 2 | DVSEC ID (1)                             |
| 0x0A   | 2    | Reserved       | Padding for DWORD alignment              |

### Vendor-specific registers (relative to DVSEC base)

| Offset | Name        | R/W | Description                                      |
|--------|-------------|-----|--------------------------------------------------|
| 0x0C   | CAPS        | RO  | F_STATE[0], F_DIRTY[1], max_ranges[11:8], pgsize[16:12] |
| 0x10   | CTRL        | WO  | Doorbell: cmd[7:0], arg[31:8]                    |
| 0x14   | STATUS      | RO  | state[7:0], error_code[15:8], quiesced[16]       |
| 0x18   | BUF_ADDR_LO | RW  | DMA buffer GPA low 32 bits                       |
| 0x1C   | BUF_ADDR_HI | RW  | DMA buffer GPA high 32 bits                      |
| 0x20   | DATA_SIZE   | RO  | State blob size in bytes                           |

### CTRL commands

| Value | Name          | Arg                | Description                          |
|-------|---------------|--------------------|--------------------------------------|
| 1     | SET_STATE     | target state       | Transition device state              |
| 2     | SAVE          | —                  | DMA-write state to buffer            |
| 3     | LOAD          | data size          | DMA-read state from buffer           |
| 4     | DIRTY_ENABLE  | —                  | Enable dirty tracking (params in buf)|
| 5     | DIRTY_DISABLE | —                  | Disable dirty tracking               |
| 6     | DIRTY_QUERY   | —                  | Query dirty bitmap (params in buf)   |
| 7     | GET_STATS     | —                  | DMA-write stats to buffer            |

### Device states

DVSEC-internal numbering, not `enum vfio_device_mig_state` values;
the driver translates between the two.

| Value | Name      | Description                                   |
|-------|-----------|-----------------------------------------------|
| 0     | ERROR     | Unrecoverable error (error_code in STATUS)    |
| 1     | STOP      | Device quiesced, no DMA                       |
| 2     | RUNNING   | Normal operation                              |
| 3     | STOP_COPY | Stopped, state available for DMA save         |
| 4     | RESUMING  | Accepting state via DMA load                  |
| 5     | PRE_COPY  | Running with dirty tracking + state snapshots |

### Error codes

When state is ERROR, STATUS[15:8] contains the error code:

| Value | Name            | Description                                    |
|-------|-----------------|------------------------------------------------|
| 1     | UNK_CMD         | Unknown CTRL command                           |
| 2     | BAD_STATE       | Invalid state transition                       |
| 3     | NO_BUFFER       | Command requires a buffer address              |
| 4     | DMA_FAILED      | DMA transfer failed                            |
| 5     | BAD_SIZE        | State blob too large or empty                  |
| 6     | BAD_MAGIC       | State blob magic mismatch                      |
| 7     | BAD_VERSION     | State blob version mismatch                    |
| 8     | TOO_MANY_RANGES | Exceeds max_ranges from CAPS                   |
| 9     | BAD_RANGE       | Invalid range (zero size, misaligned)          |
| 10    | BAD_PGSIZE      | Invalid or misaligned page size                |
| 11    | NOT_ENABLED     | Query without prior enable                     |

### State data transfer

State data is transferred via a driver-provided buffer. The driver
allocates a buffer of `IGB_VF_STATE_MAX_SIZE` (4096) bytes, writes its
GPA to `BUF_ADDR_LO/HI`, and sends a SAVE or LOAD command.

On the source, after the device enters STOP_COPY (or PRE_COPY), the
driver sends SAVE — the device writes the state blob into the buffer.
On the destination, the driver fills its buffer with the received blob
and sends LOAD with the data size in the arg field — the device reads
the blob and restores VF state.

### Dirty page tracking

**Enable**: the driver builds a `dirty_enable_req` in a DMA buffer,
sets `BUF_ADDR`, and sends DIRTY_ENABLE. One command per range. The
CAPS register advertises the maximum number of ranges (`max_ranges`
in bits [11:8]). If the VFIO core requests more ranges than the
device supports, the driver merges them with
`vfio_combine_iova_ranges()`.

DMA buffer layout for dirty enable:

```
Offset  Field              Written by  Description
0x00    len                driver      Structure size in bytes
0x04    flags              driver      Reserved, must be 0
0x08    pgsize             driver      Page granularity (must match a CAPS pgsize)
0x10    range_iova         driver      Tracked range start address
0x18    range_size         driver      Tracked range size in bytes
0x20    reserved[4]        -           Reserved, must be 0
```

**Query**: for each `log_read_and_clear` call, the driver writes the
query parameters into the DMA buffer, sets `BUF_ADDR`, and sends
DIRTY_QUERY. The device validates the range, writes the dirty bitmap
into the buffer and clears the tracked bits. Each bit in the bitmap
represents one page at the configured page size granularity.

DMA buffer layout for dirty query:

```
Offset  Field              Written by  Description
0x00    len                driver      Total buffer size in bytes
0x04    flags              driver      Reserved, must be 0
0x08    iova               driver      Query range start
0x10    size               driver      Query range size
0x18    bitmap_size        device      Bytes written to bitmap
0x1C    dirty_page_count   device      Number of set bits
0x20    dma_writes         device      Total DMA writes since enable
0x28    reserved[6]        -           Reserved, must be 0
0x40    bitmap[]           device      Dirty page bitmap
```

**Disable**: the driver sends DIRTY_DISABLE and frees the DMA buffer.

### Migration statistics

Stats are retrieved via the GET_STATS command. The driver allocates a
DMA buffer, sets `BUF_ADDR`, and sends GET_STATS — the device
DMA-writes the stats response.

Stats response layout:

```
Offset  Field              Description
0x00    dma_writes         DMA write operations tracked
0x08    dma_bytes          DMA bytes written
0x10    dirty_pages_set    Dirty pages marked since enable
0x14    dirty_pages_cleared Dirty pages cleared by queries
0x18    dirty_page_count   Current dirty pages (set - cleared)
0x1C    dirty_query_count  Number of QUERY operations
```

These stats are exposed via debugfs at
`/sys/kernel/debug/vfio/<device>/migration/dirty/stats`.

## QEMU (L0)

The emulated IGB PF with VF migration support is available on the
`igb-migration` branch at https://github.com/legoater/qemu. See the
[QEMU documentation](https://github.com/legoater/qemu/blob/igb-migration/docs/system/devices/igb-migration.rst)
for setup and usage instructions.

## Build

```sh
make KDIR=/path/to/linux
sudo make modules_install KDIR=/path/to/linux
```

Defaults to the running kernel if `KDIR` is not set. The `Kbuild`
file auto-detects missing symbols (`vfio_check_precopy_ioctl`,
`kzalloc_obj`) for compatibility with older kernels.

## Credits

Alex Williamson suggested the overall approach of a variant driver
with the "vf-migration" device property to gate the feature. Thanks
for the ever ongoing support and valuable discussions throughout
these years.

## AI disclaimer

Claude was used to analyze the IGB PF and VF internal state and
identify the pain points of a working live migration of such devices.
The generated code served as a starting point but *significant* time
was then spent cleaning up, reworking, and shaping it into a clear,
reviewable proposal.

## License

GPL-2.0-only
