#!/usr/bin/env python3
"""Keep pre-existing Friday/Codex GATT handle positions stable across upgrades.

The five released services are created sequentially by Bluedroid. Inserting
attributes into one of them moves cached handles in subsequent services even
when every characteristic UUID stays the same. This regression checks both
the enum offsets and actual database attribute order, without BLE or ESP-IDF.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

from firmware_integration_contract_test import byte_arrays, function_body, strip_cpp_comments


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def enum_names(source: str, name: str) -> list[str]:
    body = function_body(source, f"enum {name} : uint8_t")
    require(bool(body), f"missing enum {name}")
    return [entry.strip() for entry in body.split(",") if entry.strip()]


def run(root: Path) -> None:
    gatt = strip_cpp_comments((root / "main/services/codex_micro/codex_micro_gatt_db.h").read_text())
    service = strip_cpp_comments((root / "main/services/codex_micro/codex_micro_service.cpp").read_text())
    helper = strip_cpp_comments((root / "companion/macos/host_identity/host_identity.m").read_text())
    declaration = "kCharacteristicDeclarationUuid"
    primary = "kPrimaryServiceUuid"

    # Frozen relative handle offsets from the five-service firmware. The final
    # entry in each enum is its count, not an attribute.
    baseline = [
        ("DeviceInfoIndex", "kDeviceInfo", "kDeviceInfoDb", "kDiCount", [
            "kDiService", "kDiPnpDeclaration", "kDiPnpValue", "kDiManufacturerDeclaration", "kDiManufacturerValue",
        ], [primary, declaration, "kPnpUuid", declaration, "kManufacturerUuid"]),
        ("HidIndex", "kHid", "kHidDb", "kHidCount", [
            "kHidService", "kHidInfoDeclaration", "kHidInfoValue", "kHidReportMapDeclaration", "kHidReportMapValue",
            "kHidControlDeclaration", "kHidControlValue", "kHidProtocolDeclaration", "kHidProtocolValue",
            "kHidInputDeclaration", "kHidInputValue", "kHidInputCccd", "kHidInputReference",
            "kHidOutputDeclaration", "kHidOutputValue", "kHidOutputReference",
        ], [primary, declaration, "kHidInfoUuid", declaration, "kHidReportMapUuid", declaration,
            "kHidControlUuid", declaration, "kHidProtocolUuid", declaration, "kHidReportUuid", "kCccdUuid",
            "kReportReferenceUuid", declaration, "kHidReportUuid", "kReportReferenceUuid"]),
        ("BatteryIndex", "kBattery", "kBatteryDb", "kBatteryCount", [
            "kBatteryService", "kBatteryDeclaration", "kBatteryValue", "kBatteryCccd", "kBatteryFormat",
        ], [primary, declaration, "kBatteryLevelUuid", "kCccdUuid", "kPresentationFormatUuid"]),
        ("QuotaIndex", "kQuota", "kQuotaDb", "kQuotaCount", [
            "kQuotaService", "kQuotaDeclaration", "kQuotaValue",
        ], [primary, declaration, "kQuotaWriteUuid"]),
        ("FridayIndex", "kFriday", "kFridayDb", "kFridayCount", [
            "kFridayService", "kFridayContextDeclaration", "kFridayContextValue", "kFridayTransferDeclaration",
            "kFridayTransferValue", "kFridayTransferCccd", "kFridayReservedDeclaration4", "kFridayReservedValue4",
            "kFridayReservedDescriptor4", "kFridayReservedDeclaration5", "kFridayReservedValue5",
        ], [primary, declaration, "kFridayContextUuid", declaration, "kFridayTransferUuid", "kCccdUuid",
            declaration, "kFridayReservedUuid4", "kCccdUuid", declaration, "kFridayReservedUuid5"]),
    ]
    table_enum = function_body(service, "enum class TableId : uint8_t")
    table_ids = [(name, int(value)) for name, value in re.findall(r"(k\w+)\s*=\s*(\d+)", table_enum)]
    expected_order = [(entry[1], index) for index, entry in enumerate(baseline)] + [("kHostIdentity", 5), ("kCount", 6)]
    require(table_ids == expected_order, "original five services must keep their indices; identity must be appended last")
    create = function_body(service, "void Service::Impl::createTable")
    for enum, table_id, db, count, offsets, uuids in baseline:
        require(enum_names(gatt, enum) == offsets + [count], f"{enum} changed a released attribute offset/count")
        table = function_body(gatt, f"inline const esp_gatts_attr_db_t {db}")
        actual = re.findall(r"CODEX_ATTR(?:16|128)\s*\(\s*&?(k\w+)\s*,", table)
        require(actual == uuids, f"{db} changed released attribute order or count")
        require(re.search(rf"case TableId::{table_id}:\s*table = detail::{db};\s*count = detail::{count};", create) is not None,
                f"{table_id} registration must create its original database/count")
    require([len(entry[4]) for entry in baseline] == [5, 16, 5, 3, 11], "frozen baseline has changed")

    # Retired Friday slots remain solely to preserve the next service's handles.
    # Cached clients must not be able to subscribe, write acknowledgements, or
    # start an audio stream. The runtime feature and its storage are gone.
    friday_db = function_body(gatt, "inline const esp_gatts_attr_db_t kFridayDb")
    entries = re.findall(r"CODEX_ATTR(?:16|128)\s*\(((?:[^()]|\([^()]*\))*)\)", friday_db)
    require(len(entries) == 11, "Friday must retain exactly eleven allocated handles")
    require(re.search(r"kPropertyDisabled\s*=\s*0\s*;", gatt) is not None,
            "retired characteristics must expose no read/write/notify properties")
    for index in (6, 9):
        require("&kPropertyDisabled" in entries[index], "retired declarations must advertise zero properties")
    for index in (7, 8, 10):
        require(entries[index].split(",")[1].strip() == "0", "retired values and CCCD must deny all access")
    require("kFridayReserved" not in service, "retired handles must have no runtime routing or subscriptions")
    arrays = byte_arrays(gatt)
    require(bytes(arrays["kFridayReservedUuid4"]) == bytes.fromhex("fb349b5f800000800400594144495246") and
            bytes(arrays["kFridayReservedUuid5"]) == bytes.fromhex("fb349b5f800000800500594144495246"),
            "retired UUIDs must stay reserved rather than being repurposed for another operation")

    require(enum_names(gatt, "HostIdentityIndex") == ["kHostIdentityService", "kHostIdentityDeclaration", "kHostIdentityValue", "kHostIdentityCount"],
            "identity service must contain exactly its service/declaration/value")
    identity_db = function_body(gatt, "inline const esp_gatts_attr_db_t kHostIdentityDb")
    require(re.findall(r"CODEX_ATTR(?:16|128)\s*\(\s*&?(k\w+)\s*,", identity_db) == [primary, declaration, "kHostIdentityUuid"],
            "identity attributes must live only in the appended service")
    require("kHostIdentityServiceUuid" in identity_db and "ESP_GATT_PERM_WRITE_ENCRYPTED" in identity_db,
            "identity service UUID and encryption permission must be present")
    require("case TableId::kHostIdentity:" in create and "table = detail::kHostIdentityDb;" in create,
            "appended identity database must be registered")
    require("hostIdentityHandles[detail::kHostIdentityValue]" in service and
            "quotaHandles[detail::kHostIdentityValue]" not in service, "identity writes must use the appended service handles")
    for signature in ("uint16_t* Service::Impl::handlesFor", "size_t Service::Impl::handleCountFor",
                      "uint16_t Service::Impl::serviceHandleFor"):
        require("case TableId::kHostIdentity:" in function_body(service, signature), f"missing identity mapping in {signature}")
    require("createTable(TableId::kDeviceInfo)" in function_body(service, "void Service::Impl::handleRegistration"),
            "GATT creation must start from the original first service")
    created = function_body(service, "void Service::Impl::handleTableCreated")
    require("event.instanceId + 1" in created and "createTable(static_cast<TableId>(next))" in created,
            "GATT tables must still be allocated sequentially")
    require(re.search(r"kGattSchemaRevision\s*=\s*4\s*;", service) is not None,
            "appending identity must not bump the bond-clearing GATT schema revision")

    arrays = byte_arrays(gatt)
    identity_service = bytes.fromhex("045c0e1af64ebebf714ac22a664e0d7f")
    identity_value = bytes.fromhex("035c0e1af64ebebf714ac22a664e0d7f")
    require(bytes(arrays["kHostIdentityServiceUuid"]) == identity_service, "unexpected new identity service UUID")
    require(bytes(arrays["kHostIdentityUuid"]) == identity_value, "identity characteristic UUID changed")
    require(sum(bytes(value) == identity_service for value in arrays.values()) == 1, "new identity service UUID conflicts with another UUID")
    require("7F0D4E66-2AC2-4A71-BFBE-4EF61A0E5C04" in helper, "Mac helper must know appended identity service UUID")
    require("discoverCharacteristics:@[IdentityUUID()] forService:identityService" in helper,
            "Mac helper must discover identity on its own service")
    require("discoverCharacteristics:@[QuotaUUID()] forService:quotaService" in helper,
            "Mac helper must keep quota discovery on the original service")
    require("scanForPeripheralsWithServices:@[ServiceUUID()]" in helper,
            "Mac helper must retain advertised quota-service discovery")
    print("gatt_handle_compatibility_test: PASS (40 handles preserved; retired Friday slots disabled; identity unchanged)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} REPOSITORY_ROOT")
    try:
        run(Path(sys.argv[1]).resolve())
    except AssertionError as error:
        raise SystemExit(f"FAIL: {error}") from error
