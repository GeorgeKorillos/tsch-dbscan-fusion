# Compatibility changes

`contiki-neighbor-initializer.patch` adds braces around the nested `linkaddr_t`
initializer in Contiki-NG commit `713e72f54`. It retains zero initialization.
The installer applies the initializer correction consistently on both hosts.

`msp430-value-index.patch` selects an element of the `values` array in the legacy
MSP430 backend. It is applied after the mspgcc 20120911 patch and the three
additional GCC patches, to select the value rather than a pointer to that value.

The larger mspgcc and Homebrew patches are downloaded, not copied into this
folder. Their URLs and SHA-256 hashes are in `../versions/downloads.tsv`.
The Homebrew archive is pinned to commit
`124bbebceef35e802512fb293a188c7bee71093d` of
https://github.com/tgtakaoka/homebrew-mspgcc. Its repository includes a BSD-2-Clause
licence; the GNU source packages retain their own licences.

The GNU configuration scripts are taken from the GNU config mirror at
https://github.com/chipp/gnu-config, commit
`00b15927496058d23e6258a28d8996f87cf1f191`. Their copyright and licence notices
remain in the downloaded files. They replace the old host-detection scripts in
GMP, MPFR, MPC, binutils and GCC.

The installer does not change any `memory.x`. In particular it preserves the
F2617 ROM boundary at `0xFFBE`, before the BSL security word.
