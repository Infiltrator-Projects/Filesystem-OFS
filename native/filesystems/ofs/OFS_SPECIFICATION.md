# Amiga OFS format specification

The complete on-media specification, historical chronology, algorithms, field layouts and conformance guidance are maintained in the [unified technical reference](../../../docs/OFS-TECHNICAL-REFERENCE.md) and its [PDF](../../../docs/OFS.pdf). The [former specification](https://github.com/Infiltrator-Projects/Filesystem-OFS/blob/ce4c455beeb9dcc78a5dd1866805d47b02c503a4/native/filesystems/ofs/OFS_SPECIFICATION.md) is retained in Git history, not a competing current edition. Current implementation qualification remains separate from original format semantics.

## Developer conformance rule

All code or format-reference changes must identify the affected numbered section, DOS0/2/4/6 profile and block size, and add a failing-before/passing-after golden test. Do not copy FFS semantics merely because the upstream AFFS implementation handles both formats. Historical tracked qualification: [Filesystems issue #27](https://github.com/Infiltrator-Projects/Filesystems/issues/27). Production provider authority is Common V2.
