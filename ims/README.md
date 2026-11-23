# VoLTE bring-up

From the ROM root, clone PhhIms with its native audio dependencies.

```sh
git clone --recurse-submodules https://github.com/universal7880/ims.git external/phh-ims
```

The PhhIms checkout enables IMS on all universal7880 build variants. Its
bundled RRO selects it as the MmTel service. The framework overlay advertises
VoLTE capability, and CarrierConfig controls which networks expose it. Check LTE
registration, incoming and outgoing calls, two-way audio, and fallback to
circuit-switched calls.

The build includes IWLAN and QNS for Wi-Fi calling. Availability is governed
by the carrier configuration and subscription provisioning. Test incoming
and outgoing calls over IWLAN on a provisioned subscription before adding
any carrier-specific overrides.
