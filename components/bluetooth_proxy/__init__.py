import functools
import logging

import esphome.codegen as cg
from esphome.components import ble_device_base, bluetooth_connection
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACTIVE,
    CONF_ID,
    PLATFORM_BK72XX,
    PLATFORM_ESP32,
    PLATFORM_LN882X,
    PLATFORM_RP2,
)
from esphome.core import CORE
from esphome.schema_extractors import SCHEMA_EXTRACT, schema_extractor
from esphome.types import ConfigType

# The esp32 BLE stack (esp32_ble, esp32_ble_tracker) is imported lazily
# inside _esp32_config_schema()/_to_code_esp32(): importing those modules
# registers esp32-only automations (ble.enable, ble.disable, ...) as a side
# effect, and a module-scope import would leak them into every platform's
# registry the moment a config declares `bluetooth_proxy:` — degrading
# "Unable to find action" config errors into C++ compile failures.


def AUTO_LOAD(config: ConfigType | None = None) -> list[str]:
    """Components to auto-load for the platform being compiled.

    Callable with no argument so tooling that resolves AUTO_LOAD without a
    target platform (the device-builder catalog sync does exactly this) gets
    the union of every arm instead of an empty list — which is what lets it
    keep cross-referencing the esp32 BLE stack. A real build always has a
    target platform set, so it takes one of the concrete branches.
    """
    if CORE.is_esp32:
        return ["bluetooth_connection", "esp32_ble_tracker"]
    if CORE.target_platform in _HUB_PLATFORMS:
        return ["ble_device_base", "bluetooth_connection"]
    # No target platform, or one this component does not support: tooling
    # resolving the manifest (including the host-pinned dependency resolver) —
    # expose every arm so the closure keeps the esp32 BLE stack.
    return [
        "ble_device_base",
        "bluetooth_connection",
        "esp32_ble_tracker",
    ]


# Platforms with an in-tree ble_device_base BLE tracker hub whose controller
# supports active scanning — every current client (aioesphomeapi, bleak-esphome,
# Home Assistant) assumes an ESPHome proxy can scan actively, so a passive-only
# hub must not be admitted (it would be misdriven).
# Coupled to bluetooth_connection: platforms with a GATT backend are also
# listed in its _PLATFORM_BACKENDS registry, HUB_MAX_CONNECTIONS, and
# FILTER_SOURCE_FILES hub entry.
_HUB_PLATFORMS = (PLATFORM_BK72XX, PLATFORM_LN882X, PLATFORM_RP2)

DEPENDENCIES = ["api"]
CODEOWNERS = ["@jesserockz", "@bdraco"]

_LOGGER = logging.getLogger(__name__)

CONF_CONNECTION_SLOTS = "connection_slots"
CONF_CACHE_SERVICES = "cache_services"
CONF_CONNECTIONS = "connections"
CONF_RSSI_THRESHOLD = "rssi_threshold"
CONF_RSSI_FLOOR = "rssi_floor"
CONF_RSSI_MAC_ALLOWLIST = "rssi_mac_allowlist"
CONF_RSSI_IRK = "rssi_irk"
CONF_RSSI_SERVICE_UUID = "rssi_service_uuid"
CONF_IRKS = "irks"
CONF_ALLOW_ESPRESSIF = "allow_espressif"
CONF_NAME_BLOCKLIST = "name_blocklist"
CONF_MAC_ALLOWLIST = "mac_allowlist"
CONF_MAC_BLOCKLIST = "mac_blocklist"
CONF_ALLOWLIST_EXCLUSIVE = "allowlist_exclusive"
CONF_MANUFACTURER_BLOCKLIST = "manufacturer_blocklist"
CONF_DROP_NON_RESOLVABLE = "drop_non_resolvable"
CONF_ALLOW_HOMEKIT = "allow_homekit"
CONF_ALLOW_IBEACON = "allow_ibeacon"
CONF_ALLOW_FINDMY = "allow_findmy"
CONF_UUID = "uuid"
CONF_MAJOR = "major"
CONF_MINOR = "minor"
CONF_RSSI = "rssi"
CONF_SERVICE_UUID_ALLOWLIST = "service_uuid_allowlist"


def _validate_irk(value: str) -> str:
    """One 16-byte Identity Resolving Key, as 32 hex chars.

    Accepts the separator styles people paste (colons, dashes, spaces) and
    normalises to bare lowercase hex; the C++ side parses the concatenated
    blob once at boot.
    """
    value = cv.string_strict(value)
    stripped = value.replace(":", "").replace("-", "").replace(" ", "").lower()
    if len(stripped) != 32:
        raise cv.Invalid(
            f"IRK must be 16 bytes (32 hex characters), got {len(stripped)}"
        )
    if any(c not in "0123456789abcdef" for c in stripped):
        raise cv.Invalid("IRK must be hexadecimal")
    return stripped


def _validate_service_uuid(value: int | str) -> int | str:
    """A service UUID to allowlist: either a 16-bit short or a full 128-bit UUID.

    Returns either an int (short) or a 32-char lowercase hex string (long), and
    the codegen dispatches on the type. A 128-bit UUID written in its Base-UUID
    long form is NOT folded to a short here - the C++ side compares against the
    base at match time, so both spellings work either way.
    """
    if isinstance(value, int):
        return cv.hex_uint16_t(value)
    text = cv.string_strict(value).strip()
    stripped = text.replace("-", "").replace(":", "").replace(" ", "").lower()
    if len(stripped) == 32:
        if any(c not in "0123456789abcdef" for c in stripped):
            raise cv.Invalid("128-bit service UUID must be hexadecimal")
        return stripped
    # Anything else: let the 16-bit validator produce the error message.
    return cv.hex_uint16_t(value)


DEFAULT_CONNECTION_SLOTS = 3

bluetooth_proxy_ns = cg.esphome_ns.namespace("bluetooth_proxy")

BluetoothProxy = bluetooth_proxy_ns.class_("BluetoothProxy", cg.Component)

# Mirrors esp32_ble.IDF_MAX_CONNECTIONS (the loosest platform cap): the esp32
# schema builder asserts the two agree, tests/component_tests/bluetooth_proxy/
# pins them together, and the outer walkable schema uses it as the
# connection_slots bound (per-platform schemas tighten it).
_IDF_MAX_CONNECTIONS = 9


@functools.cache
def _esp32_config_schema() -> cv.All:
    """Build the esp32 schema, importing the esp32 BLE stack only when used."""
    from esphome.components import esp32_ble, esp32_ble_tracker

    if esp32_ble.IDF_MAX_CONNECTIONS != _IDF_MAX_CONNECTIONS:
        raise cv.Invalid(
            f"bluetooth_proxy's connection-slot limit mirror "
            f"({_IDF_MAX_CONNECTIONS}) is out of sync with "
            f"esp32_ble.IDF_MAX_CONNECTIONS ({esp32_ble.IDF_MAX_CONNECTIONS}); "
            f"update _IDF_MAX_CONNECTIONS in bluetooth_proxy/__init__.py"
        )

    CONNECTION_SCHEMA = bluetooth_connection.hub_connection_schema(PLATFORM_ESP32)

    def validate_connections(config: ConfigType) -> ConfigType:
        if CONF_CONNECTIONS in config:
            if not config[CONF_ACTIVE]:
                raise cv.Invalid(
                    "Connections can only be used if the proxy is set to active"
                )
        elif config[CONF_ACTIVE]:
            connection_slots: int = config[CONF_CONNECTION_SLOTS]
            esp32_ble.consume_connection_slots(connection_slots, "bluetooth_proxy")(
                config
            )

            return {
                **config,
                CONF_CONNECTIONS: [
                    CONNECTION_SCHEMA({}) for _ in range(connection_slots)
                ],
            }
        return config

    return cv.All(
        (
            cv.Schema(
                {
                    **_COMMON_SCHEMA_KEYS,
                    cv.Optional(CONF_ACTIVE, default=True): cv.boolean,
                    cv.Optional(CONF_CACHE_SERVICES, default=True): cv.boolean,
                    cv.Optional(
                        CONF_CONNECTION_SLOTS,
                        default=DEFAULT_CONNECTION_SLOTS,
                    ): cv.All(
                        cv.positive_int,
                        cv.Range(min=1, max=esp32_ble.IDF_MAX_CONNECTIONS),
                    ),
                    cv.Optional(CONF_CONNECTIONS): cv.All(
                        cv.ensure_list(CONNECTION_SCHEMA),
                        cv.Length(min=1, max=esp32_ble.IDF_MAX_CONNECTIONS),
                    ),
                }
            )
            .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
            .extend(cv.COMPONENT_SCHEMA)
        ),
        validate_connections,
        _validate_rssi_floor,
    )


# One entry of the allow_ibeacon list. `minor` accepts a single value or a
# list; omitting it exempts the whole major. Nested rather than flat
# ibeacon_major/ibeacon_minor keys because a minor is only meaningful inside a
# major - flat keys cannot express "major 1 minor 7 AND major 10 minor 3", and
# silently read as one global pair.
# Mirrors BluetoothProxy::IBEACON_RSSI_INHERIT.
_IBEACON_RSSI_INHERIT = -128


def _validate_ibeacon_uuid(value: str) -> str:
    """An iBeacon proximity UUID, normalised to 32 lowercase hex characters."""
    return cv.uuid(value).hex


def _validate_ibeacon_rule(rule: ConfigType) -> ConfigType:
    if CONF_UUID not in rule and CONF_MAJOR not in rule:
        raise cv.Invalid(
            f"An allow_ibeacon rule needs a '{CONF_UUID}', a '{CONF_MAJOR}', or "
            f"both. To exempt every iBeacon, use 'allow_ibeacon: true'."
        )
    if CONF_MINOR in rule and CONF_MAJOR not in rule:
        raise cv.Invalid(
            f"'{CONF_MINOR}' is only meaningful inside a '{CONF_MAJOR}'",
            path=[CONF_MINOR],
        )
    return rule


_IBEACON_FILTER_SCHEMA = cv.All(
    cv.Schema(
        {
            # Whose beacon it is. major/minor are small integers every vendor
            # starts counting from 1, so a rule without a uuid admits anybody's
            # beacon that was left on its defaults - at this rule's RSSI limit.
            cv.Optional(CONF_UUID): _validate_ibeacon_uuid,
            cv.Optional(CONF_MAJOR): cv.uint16_t,
            cv.Optional(CONF_MINOR): cv.ensure_list(cv.uint16_t),
            # This filter's own RSSI limit, overriding BOTH rssi_threshold and
            # rssi_floor for adverts it matches. Omitted = inherit: the rule exempts
            # the advert from manufacturer_blocklist and nothing else, so the
            # distance rules still apply. -127 forwards at any strength.
            cv.Optional(CONF_RSSI, default=_IBEACON_RSSI_INHERIT): cv.Any(
                cv.int_range(min=-127, max=0),
                cv.int_range(min=_IBEACON_RSSI_INHERIT, max=_IBEACON_RSSI_INHERIT),
            ),
        }
    ),
    _validate_ibeacon_rule,
)


# allow_findmy: `true` exempts Apple FindMy adverts (Offline Finding subtype
# 0x12, and AirPods proximity-pairing subtype 0x07, which AirPods near their
# owner send from the same rotated address) from manufacturer_blocklist and
# nothing more; a mapping with `rssi`
# gives the rule its own limit, resolved exactly like an iBeacon rule's
# (overrides rssi_threshold and rssi_floor for the adverts it matches).
_FINDMY_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_RSSI, default=_IBEACON_RSSI_INHERIT): cv.Any(
            cv.int_range(min=_IBEACON_RSSI_INHERIT, max=_IBEACON_RSSI_INHERIT),
            cv.int_range(min=-127, max=0),
        ),
    }
)


def _validate_allow_findmy(value: bool | ConfigType) -> bool | ConfigType:
    if isinstance(value, bool):
        return value
    return _FINDMY_SCHEMA(value)


def _findmy_to_code(var: cg.MockObj, config: ConfigType) -> None:
    """Emit the allow_findmy config."""
    value = config.get(CONF_ALLOW_FINDMY, False)
    if value is False:
        return
    cg.add(var.set_allow_findmy(True))
    rssi = _IBEACON_RSSI_INHERIT if value is True else value[CONF_RSSI]
    cg.add(var.set_findmy_rssi(rssi))


def _validate_allow_ibeacon(value: bool | list[ConfigType]) -> bool | list[ConfigType]:
    """Accept `true`/`false`, or a list of major/minor filters."""
    if isinstance(value, bool):
        return value
    return cv.ensure_list(_IBEACON_FILTER_SCHEMA)(value)


def _validate_rssi_floor(config: ConfigType) -> ConfigType:
    """Reject a floor stricter than the threshold it is meant to backstop.

    rssi_floor runs before categorisation and applies to every advertisement;
    every other limit is applied afterwards to one category. The floor is
    therefore only meaningful while it is the loosest of them. A floor above a
    category's limit would silently take over as that category's effective
    filter, and the category limit would stop meaning anything - so fail loudly
    rather than quietly changing behaviour.
    """
    floor = config.get(CONF_RSSI_FLOOR, -127)
    threshold = config.get(CONF_RSSI_THRESHOLD, -127)
    for key in (CONF_RSSI_MAC_ALLOWLIST, CONF_RSSI_IRK, CONF_RSSI_SERVICE_UUID):
        limit = config.get(key, -127)
        if limit != -127 and floor != -127 and limit < floor:
            raise cv.Invalid(
                f"{key} ({limit}) is below {CONF_RSSI_FLOOR} ({floor}), so it "
                f"can never fire: the floor has already dropped anything that "
                f"weak. Raise it above the floor, or remove it.",
                path=[key],
            )
    # -127 is "no threshold", not a very loose one: a floor on its own ("drop
    # only what is too weak to mean anything") is a legitimate config, and used
    # to be rejected here with a message about a threshold nobody had set.
    if floor != -127 and threshold != -127 and floor > threshold:
        raise cv.Invalid(
            f"{CONF_RSSI_FLOOR} ({floor}) must be at or below "
            f"{CONF_RSSI_THRESHOLD} ({threshold}): the floor is an absolute "
            f"backstop applied ahead of the allowlists, so a floor stricter "
            f"than the threshold would override it for every device.",
            path=[CONF_RSSI_FLOOR],
        )
    return config


def _validate_no_active(config: ConfigType) -> ConfigType:
    if config[CONF_ACTIVE]:
        raise cv.Invalid(
            "Active connections are not supported on this platform; the proxy "
            "forwards advertisements only (set active: false)"
        )
    return config


@functools.cache
def _rp2_config_schema() -> cv.All:
    """Full proxy on the rp2 BLE hub: active connections through the BTstack
    GATT client backend in bluetooth_connection. Multi-slot builds replace the
    prebuilt library's one-client BTstack pools via linker --wrap, owned by
    rp2040_ble and requested when a second backend registers."""
    connection_schema = bluetooth_connection.hub_connection_schema(PLATFORM_RP2)

    def populate_connections(config: ConfigType) -> ConfigType:
        from esphome.components import rp2040_ble

        # One wrapper + backend pair per slot, declared during validation so
        # their ids exist for codegen (the esp32 arm's `connections` pattern).
        if not config[CONF_ACTIVE]:
            return config
        connection_slots: int = config[CONF_CONNECTION_SLOTS]
        rp2040_ble.consume_connection_slots(connection_slots, "bluetooth_proxy")(config)
        return {
            **config,
            CONF_CONNECTIONS: [connection_schema({}) for _ in range(connection_slots)],
        }

    max_conn = bluetooth_connection.HUB_MAX_CONNECTIONS[PLATFORM_RP2]
    schema = (
        cv.Schema(
            {
                **_COMMON_SCHEMA_KEYS,
                cv.Optional(CONF_ACTIVE, default=True): cv.boolean,
                cv.Optional(
                    CONF_CONNECTION_SLOTS,
                    default=min(DEFAULT_CONNECTION_SLOTS, max_conn),
                ): cv.All(
                    cv.positive_int,
                    cv.Range(
                        min=1,
                        max=max_conn,
                        msg=f"rp2 supports at most {max_conn} connection slot(s); "
                        "the BTstack pool overrides in rp2040_ble are sized "
                        f"for {max_conn}",
                    ),
                ),
            }
        )
        .extend(
            # ble_hub_id with the friendly no-tracker-configured guard.
            ble_device_base.BLE_DEVICE_SCHEMA
        )
        .extend(cv.COMPONENT_SCHEMA)
    )
    return cv.All(schema, populate_connections, _validate_rssi_floor)


def _ibeacon_to_code(var: cg.MockObj, config: ConfigType) -> None:
    """Emit the allow_ibeacon config."""
    value = config[CONF_ALLOW_IBEACON]
    if value is False:
        return
    if value is True:
        # A bare `true` exempts every iBeacon from the blocklist and nothing
        # more; the distance rules still apply, same as an inheriting filter.
        cg.add(var.set_allow_ibeacon(True))
        cg.add(var.set_ibeacon_any_rssi(_IBEACON_RSSI_INHERIT))
        return
    # A list scopes the exemption, so the unscoped flag stays off.
    cg.add(var.set_allow_ibeacon(False))
    for entry in value:
        # nullptr / -1 mean "any". The C++ side orders rules most-specific-first.
        uuid = entry.get(CONF_UUID, cg.nullptr)
        major = entry.get(CONF_MAJOR, -1)
        rssi = entry[CONF_RSSI]
        for minor in entry.get(CONF_MINOR) or [-1]:
            cg.add(var.add_ibeacon_rule(uuid, major, minor, rssi))


def _irk_and_oui_to_code(var: cg.MockObj, config: ConfigType) -> None:
    """Wire up the IRK list and the Espressif-OUI allowance.

    The IRKs are handed over as one concatenated hex blob and parsed once in
    setup(); that avoids emitting a codegen array per key and keeps this fork's
    delta small. The blob is freed after parsing.
    """
    irks = config[CONF_IRKS]
    if irks:
        cg.add(var.set_irks_hex("".join(irks)))
    cg.add(var.set_allow_espressif(config[CONF_ALLOW_ESPRESSIF]))
    for needle in config[CONF_NAME_BLOCKLIST]:
        # Lowercased here so the match loop stays a plain case-sensitive compare.
        cg.add(var.add_blocked_name(needle.lower()))
    for mac in config[CONF_MAC_ALLOWLIST]:
        cg.add(var.add_allowed_mac(mac.as_hex))
    for mac in config[CONF_MAC_BLOCKLIST]:
        cg.add(var.add_blocked_mac(mac.as_hex))
    for uuid in config[CONF_SERVICE_UUID_ALLOWLIST]:
        if isinstance(uuid, str):
            cg.add(var.add_allowed_service_uuid128(uuid))
        else:
            cg.add(var.add_allowed_service_uuid(uuid))
    for company in config[CONF_MANUFACTURER_BLOCKLIST]:
        cg.add(var.add_blocked_manufacturer(company))
    cg.add(var.set_drop_non_resolvable(config[CONF_DROP_NON_RESOLVABLE]))
    cg.add(var.set_allowlist_exclusive(config[CONF_ALLOWLIST_EXCLUSIVE]))
    cg.add(var.set_allow_homekit(config[CONF_ALLOW_HOMEKIT]))


async def _connections_to_code(var: cg.MockObj, config: ConfigType) -> None:
    """One wrapper + backend pair per slot; the platform-specific backend
    registration lives in bluetooth_connection.new_gatt_backend()."""
    connections = config.get(CONF_CONNECTIONS, [])
    # The api component sizes BluetoothConnectionsFreeResponse.allocated with
    # this define whenever a proxy is present (zero on advertisement-only
    # hubs); sized here so it can never diverge from the loop below.
    cg.add_define("BLUETOOTH_PROXY_MAX_CONNECTIONS", len(connections))
    if connections:
        # Gates the connection and GATT half of the API surface. A proxy
        # without slots omits FEATURE_ACTIVE_CONNECTIONS, so a client never
        # sends those requests and their handlers and encoders are dead.
        cg.add_define("USE_BLUETOOTH_PROXY_CONNECTIONS")
    for connection_conf in connections:
        backend = await bluetooth_connection.new_gatt_backend(connection_conf)
        connection = cg.new_Pvariable(connection_conf[CONF_ID])
        cg.add(connection.set_backend(backend))
        cg.add(var.register_connection(connection))


# Per-platform schema builders; every key of
# bluetooth_connection.HUB_MAX_CONNECTIONS needs an entry here (pinned by
# tests/component_tests/bluetooth_proxy/). Connection codegen is shared.
_GATT_HUB_SCHEMAS = {PLATFORM_RP2: _rp2_config_schema}


# Keys every platform arm declares identically; each arm spreads this dict so
# the shared surface cannot drift. CONF_ACTIVE stays per-arm: its default
# differs (esp32 True, rp2 True, advertisement-only False).
# The default matches upstream behaviour (forward everything); every arm shares
# it, so unlike CONF_ACTIVE it belongs in the common keys.
_COMMON_SCHEMA_KEYS = {
    cv.GenerateID(): cv.declare_id(BluetoothProxy),
    cv.Optional(CONF_RSSI_THRESHOLD, default=-127): cv.int_range(min=-127, max=0),
    # Absolute floor, applied before the allowlists rather than after them, so
    # it bounds mac_allowlist / service_uuid_allowlist too. -127 disables it and
    # is the default, keeping an unconfigured build on upstream behaviour.
    cv.Optional(CONF_RSSI_FLOOR, default=-127): cv.int_range(min=-127, max=0),
    # Per-category limits for the three protection categories. -127 (default)
    # means the category is bounded only by rssi_floor, which is how the
    # component behaved before these existed.
    cv.Optional(CONF_RSSI_MAC_ALLOWLIST, default=-127): cv.int_range(min=-127, max=0),
    cv.Optional(CONF_RSSI_IRK, default=-127): cv.int_range(min=-127, max=0),
    cv.Optional(CONF_RSSI_SERVICE_UUID, default=-127): cv.int_range(min=-127, max=0),
    # Empty list = no IRK gating, so the default stays upstream behaviour.
    cv.Optional(CONF_IRKS, default=[]): cv.ensure_list(_validate_irk),
    cv.Optional(CONF_ALLOW_ESPRESSIF, default=True): cv.boolean,
    # Deliberately a blocklist, not an allowlist: most advertisements carry no
    # local name at all (phones and watches omit it), so allowlisting by name
    # would discard nearly everything.
    cv.Optional(CONF_NAME_BLOCKLIST, default=[]): cv.ensure_list(
        cv.All(cv.string_strict, cv.Length(min=1, max=29))
    ),
    # Addresses that bypass every filter below. Matched on the address itself
    # rather than the advertised name, because the devices worth protecting
    # (beacon tags) generally advertise no local name at all.
    cv.Optional(CONF_MAC_ALLOWLIST, default=[]): cv.ensure_list(cv.mac_address),
    # Addresses this proxy ignores outright. Beats mac_allowlist and everything
    # else - for taking one proxy out of contention for a bonded device without
    # making it a single-purpose bridge.
    cv.Optional(CONF_MAC_BLOCKLIST, default=[]): cv.ensure_list(cv.mac_address),
    # Makes mac_allowlist exclusive rather than a bypass: nothing else forwards.
    cv.Optional(CONF_ALLOWLIST_EXCLUSIVE, default=False): cv.boolean,
    # Bluetooth SIG company identifiers to discard (e.g. 0x004C Apple). Devices
    # matched by mac_allowlist or by an IRK are exempt.
    cv.Optional(CONF_MANUFACTURER_BLOCKLIST, default=[]): cv.ensure_list(
        cv.hex_uint16_t
    ),
    # Off by default so an unconfigured build matches upstream behaviour.
    cv.Optional(CONF_DROP_NON_RESOLVABLE, default=False): cv.boolean,
    # Exempt HomeKit (HAP, Apple company id + subtype 0x06) from
    # manufacturer_blocklist. On by default: blocklisting Apple for phone and
    # AirTag noise should not silently kill HomeKit BLE accessories.
    cv.Optional(CONF_ALLOW_HOMEKIT, default=True): cv.boolean,
    # iBeacon is Apple manufacturer data (subtype 0x02), so blocklisting 0x004C
    # takes every iBeacon with it. Off by default - unlike HomeKit, an iBeacon
    # is usually exactly the noise the blocklist is there to kill. Turn it on
    # when something you own beacons, e.g. ESPHome proxies advertising for BLE
    # positioning self-calibration.
    cv.Optional(CONF_ALLOW_IBEACON, default=False): _validate_allow_ibeacon,
    cv.Optional(CONF_ALLOW_FINDMY, default=False): _validate_allow_findmy,
    # 16-bit service UUIDs that bypass every filter, including the address-type
    # tests above. The companion to mac_allowlist for devices whose address is
    # not knowable in advance: anything advertising a transient pairing service
    # (Matter commissioning is 0xFFF6) does so from a rotating private address,
    # which drop_non_resolvable and the IRK test would otherwise discard.
    # Empty by default, which keeps upstream behaviour and the cheaper filter
    # ordering (the payload walk is skipped entirely when unused).
    # Accepts 16-bit shorts (0xFFF6) and full 128-bit UUIDs
    # ("00467768-6228-2272-4663-277478268000", Improv Wi-Fi).
    cv.Optional(CONF_SERVICE_UUID_ALLOWLIST, default=[]): cv.ensure_list(
        _validate_service_uuid
    ),
}

# Advertisement-only proxy on a neutral BLE hub: the hub's raw-advertisement
# callback feeds the same API batching, no connection stack compiled.
_BLE_HUB_CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            **_COMMON_SCHEMA_KEYS,
            cv.Optional(CONF_ACTIVE, default=False): cv.boolean,
        }
    )
    .extend(
        # ble_hub_id with the friendly no-tracker-configured guard.
        ble_device_base.BLE_DEVICE_SCHEMA
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_no_active,
    _validate_rssi_floor,
)


@schema_extractor("schema")
def _validate_platform(config: ConfigType) -> ConfigType:
    """Apply the schema for the platform actually being compiled.

    Three-way dispatch: esp32 gets the full GATT proxy, HUB_MAX_CONNECTIONS
    platforms get their _GATT_HUB_SCHEMAS arm, the remaining hub platforms get
    the advertisement-only shape; unsupported keys were already rejected by
    name in _reject_unsupported_connection_keys.
    """
    if config is SCHEMA_EXTRACT:
        # The language-schema dumper runs without a platform. Expose the esp32
        # shape so `connections`, the ids and every default stay in the
        # generated schema the editor and dashboard consume.
        return _esp32_config_schema()
    if CORE.is_esp32:
        return _esp32_config_schema()(config)
    if CORE.target_platform not in _HUB_PLATFORMS:
        # Fail here with the actual reason. Without this gate the error surfaces
        # later as an unresolvable hub ID ("Are you missing a hub declaration?")
        # on platforms where no hub component can be declared.
        full = ", ".join(["esp32", *sorted(bluetooth_connection.HUB_MAX_CONNECTIONS)])
        adv_only = ", ".join(
            sorted(set(_HUB_PLATFORMS) - set(bluetooth_connection.HUB_MAX_CONNECTIONS))
        )
        raise cv.Invalid(
            f"bluetooth_proxy is not supported on {CORE.target_platform}: no "
            "active-scan-capable BLE tracker hub is available for this "
            f"platform. It runs on {full} (full proxy) and {adv_only} "
            "(advertisement-only)."
        )
    if CORE.target_platform in bluetooth_connection.HUB_MAX_CONNECTIONS:
        return _GATT_HUB_SCHEMAS[CORE.target_platform]()(config)
    return _BLE_HUB_CONFIG_SCHEMA(config)


def _reject_unsupported_connection_keys(config: ConfigType) -> ConfigType:
    """Reject connection options a platform does not support, by name.

    GATT hub platforms keep connection_slots but reject the esp32-only keys;
    advertisement-only hubs reject all three. Runs before the walkable schema
    below so the user gets "this option does not exist here" instead of a
    value-range error implying the option works.
    """
    if not isinstance(config, dict) or CORE.is_esp32 or CORE.target_platform is None:
        return config
    if CORE.target_platform not in _HUB_PLATFORMS:
        # No proxy of any kind exists here: fall through so _validate_platform
        # reports "not supported on {platform}" instead of a key-level message
        # implying an advertisement-only proxy is available.
        return config
    if CORE.target_platform in bluetooth_connection.HUB_MAX_CONNECTIONS:
        # Full proxy: connection_slots is real here; the per-connection list
        # exists internally but carries no user options, and the Bluedroid
        # NVS service cache is esp32-only.
        rejected = {
            CONF_CONNECTIONS: (
                "has no per-connection options on this platform; use "
                "'connection_slots' to set the count"
            ),
            CONF_CACHE_SERVICES: "is esp32-only (Bluedroid NVS service cache)",
        }
    else:
        reason = (
            "requires active connection support; this platform runs the "
            "advertisement-only proxy and has no such option"
        )
        rejected = dict.fromkeys(
            (CONF_CONNECTION_SLOTS, CONF_CACHE_SERVICES, CONF_CONNECTIONS), reason
        )
    for key, reason in rejected.items():
        if key in config:
            raise cv.Invalid(f"'{key}' {reason}", path=[key])
    return config


# CONFIG_SCHEMA stays a statically walkable schema: tooling (the dashboard's
# field-range extractor among others) introspects it to discover options and
# their bounds, which a bare dispatch function would hide. It carries the scalar
# keys with no defaults; _validate_platform then runs the real per-platform
# schema, which applies the defaults and rejects options the platform does not
# support.
#
# It deliberately does NOT declare `connections`: this outer schema runs before
# the per-platform one, so any key it transforms is transformed twice. Running
# CONNECTION_SCHEMA twice re-validates an already-generated ID through
# declare_id(), which (unlike use_id) has no guard for an ID instance and
# rejects it as empty. extra=ALLOW_EXTRA passes `connections` through untouched
# for _ESP32_CONFIG_SCHEMA to validate exactly once.
CONFIG_SCHEMA = cv.All(
    _reject_unsupported_connection_keys,
    cv.Schema(
        {
            cv.Optional(CONF_ACTIVE): cv.boolean,
            cv.Optional(CONF_CACHE_SERVICES): cv.boolean,
            cv.Optional(CONF_RSSI_THRESHOLD): cv.int_range(min=-127, max=0),
            cv.Optional(CONF_RSSI_FLOOR): cv.int_range(min=-127, max=0),
            cv.Optional(CONF_ALLOW_IBEACON): _validate_allow_ibeacon,
            cv.Optional(CONF_ALLOW_FINDMY): _validate_allow_findmy,
            cv.Optional(CONF_RSSI_MAC_ALLOWLIST): cv.int_range(min=-127, max=0),
            cv.Optional(CONF_RSSI_IRK): cv.int_range(min=-127, max=0),
            cv.Optional(CONF_RSSI_SERVICE_UUID): cv.int_range(min=-127, max=0),
            # Bounded by the loosest platform cap so range walkers (the
            # device-builder field-range sync) see a real Range; the
            # per-platform schemas tighten it (1 on rp2) with their own error.
            cv.Optional(CONF_CONNECTION_SLOTS): cv.All(
                cv.positive_int,
                cv.Range(min=1, max=_IDF_MAX_CONNECTIONS),
            ),
        },
        extra=cv.ALLOW_EXTRA,
    ),
    _validate_platform,
)


async def _to_code_esp32(config: ConfigType) -> None:
    from esphome.components import esp32_ble, esp32_ble_tracker
    from esphome.components.esp32 import add_idf_sdkconfig_option
    from esphome.components.esp32_ble import BTLoggers

    # Register the loggers this component needs
    esp32_ble.register_bt_logger(BTLoggers.GATT, BTLoggers.L2CAP, BTLoggers.SMP)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_active(config[CONF_ACTIVE]))
    cg.add(var.set_rssi_threshold(config[CONF_RSSI_THRESHOLD]))
    cg.add(var.set_rssi_floor(config[CONF_RSSI_FLOOR]))
    cg.add(var.set_rssi_mac_allowlist(config[CONF_RSSI_MAC_ALLOWLIST]))
    # The pre-gate is derived from all of this in C++ (recompute_gate_), so that
    # runtime setters keep it correct; nothing to emit for it here.
    _ibeacon_to_code(var, config)
    _findmy_to_code(var, config)
    cg.add(var.set_rssi_irk(config[CONF_RSSI_IRK]))
    cg.add(var.set_rssi_service_uuid(config[CONF_RSSI_SERVICE_UUID]))
    _irk_and_oui_to_code(var, config)
    tracker = await cg.get_variable(config[esp32_ble_tracker.CONF_ESP32_BLE_ID])
    cg.add(var.set_ble_hub(tracker))

    # Compiles the scanner-state push slot into the tracker and the matching
    # registration into the proxy; the other hubs are polled instead.
    cg.add_define("USE_BLE_SCANNER_STATE_CALLBACK")

    await _connections_to_code(var, config)

    if config.get(CONF_CACHE_SERVICES):
        add_idf_sdkconfig_option("CONFIG_BT_GATTC_CACHE_NVS_FLASH", True)


async def _to_code_ble_hub(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_active(config[CONF_ACTIVE]))
    cg.add(var.set_rssi_threshold(config[CONF_RSSI_THRESHOLD]))
    cg.add(var.set_rssi_floor(config[CONF_RSSI_FLOOR]))
    cg.add(var.set_rssi_mac_allowlist(config[CONF_RSSI_MAC_ALLOWLIST]))
    # The pre-gate is derived from all of this in C++ (recompute_gate_), so that
    # runtime setters keep it correct; nothing to emit for it here.
    _ibeacon_to_code(var, config)
    _findmy_to_code(var, config)
    cg.add(var.set_rssi_irk(config[CONF_RSSI_IRK]))
    cg.add(var.set_rssi_service_uuid(config[CONF_RSSI_SERVICE_UUID]))
    _irk_and_oui_to_code(var, config)
    hub = await cg.get_variable(config[ble_device_base.CONF_BLE_HUB_ID])
    cg.add(var.set_ble_hub(hub))

    await _connections_to_code(var, config)


async def to_code(config: ConfigType) -> None:
    if CORE.is_esp32:
        await _to_code_esp32(config)
    else:
        await _to_code_ble_hub(config)

    # Define batch size for BLE advertisements
    # Each advertisement is up to 80 bytes when packaged (including protocol overhead)
    # 16 advertisements × 80 bytes (worst case) = 1280 bytes out of ~1320 bytes usable payload
    # This achieves ~97% WiFi MTU utilization while staying under the limit
    cg.add_define("BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE", 16)

    cg.add_define("USE_BLUETOOTH_PROXY")
