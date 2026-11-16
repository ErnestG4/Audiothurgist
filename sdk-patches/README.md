# Local patches to alchemy-sdk v0.13.0

Audiothurgist builds against Hermetic Modular's
[alchemy-sdk](https://github.com/hermetic-modular/alchemy-sdk) v0.13.0 with
these two patches applied (`docs/sdk-quirks.md` has the why):

1. `0001-…-boost.patch`: `AlchemyLabV2::Init(sample_rate, block_size, boost)`,
   so the firmware runs the Seed at 480 MHz.
2. `0002-…-CDC-t.patch`: the shared USB device and the HostLink CDC transport
   built at first use in zeroed storage, which takes 19.4 KB out of SRAM on a
   BOOT_SRAM build. Without it the firmware does not fit.

To set up the SDK beside this repo:

```sh
git clone https://github.com/hermetic-modular/alchemy-sdk.git ../alchemy-sdk-0.13
cd ../alchemy-sdk-0.13
git switch -c local/v0.13 v0.13.0
git am ../Audiothurgist/sdk-patches/*.patch
git submodule update --init --recursive
make -C vendor/libDaisy
```

**Licence.** The SDK is MIT-licensed by Hermetic Modular LLC, and the code
these patches modify stays under its licence. The changes the patches make are
part of Audiothurgist, under AGPL-3.0 like the rest of this repository.
