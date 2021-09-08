// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/browser/serial/serial_chooser_context.h"

#include <memory>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/containers/contains.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "content/public/browser/device_service.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace electron {

constexpr char kPortNameKey[] = "name";
constexpr char kTokenKey[] = "token";
#if defined(OS_WIN)
constexpr char kDeviceInstanceIdKey[] = "device_instance_id";
#else
constexpr char kVendorIdKey[] = "vendor_id";
constexpr char kProductIdKey[] = "product_id";
constexpr char kSerialNumberKey[] = "serial_number";
#if defined(OS_MAC)
constexpr char kUsbDriverKey[] = "usb_driver";
#endif  // defined(OS_MAC)
#endif  // defined(OS_WIN)

std::string EncodeToken(const base::UnguessableToken& token) {
  const uint64_t data[2] = {token.GetHighForSerialization(),
                            token.GetLowForSerialization()};
  std::string buffer;
  base::Base64Encode(
      base::StringPiece(reinterpret_cast<const char*>(&data[0]), sizeof(data)),
      &buffer);
  return buffer;
}

base::UnguessableToken DecodeToken(base::StringPiece input) {
  std::string buffer;
  if (!base::Base64Decode(input, &buffer) ||
      buffer.length() != sizeof(uint64_t) * 2) {
    return base::UnguessableToken();
  }

  const uint64_t* data = reinterpret_cast<const uint64_t*>(buffer.data());
  return base::UnguessableToken::Deserialize(data[0], data[1]);
}

base::Value PortInfoToValue(const device::mojom::SerialPortInfo& port) {
  base::Value value(base::Value::Type::DICTIONARY);
  if (port.display_name && !port.display_name->empty())
    value.SetStringKey(kPortNameKey, *port.display_name);
  else
    value.SetStringKey(kPortNameKey, port.path.LossyDisplayName());

  if (!SerialChooserContext::CanStorePersistentEntry(port)) {
    value.SetStringKey(kTokenKey, EncodeToken(port.token));
    return value;
  }

#if defined(OS_WIN)
  // Windows provides a handy device identifier which we can rely on to be
  // sufficiently stable for identifying devices across restarts.
  value.SetStringKey(kDeviceInstanceIdKey, port.device_instance_id);
#else
  DCHECK(port.has_vendor_id);
  value.SetIntKey(kVendorIdKey, port.vendor_id);
  DCHECK(port.has_product_id);
  value.SetIntKey(kProductIdKey, port.product_id);
  DCHECK(port.serial_number);
  value.SetStringKey(kSerialNumberKey, *port.serial_number);

#if defined(OS_MAC)
  DCHECK(port.usb_driver_name && !port.usb_driver_name->empty());
  value.SetStringKey(kUsbDriverKey, *port.usb_driver_name);
#endif  // defined(OS_MAC)
#endif  // defined(OS_WIN)
  return value;
}

SerialChooserContext::SerialChooserContext(
    ElectronBrowserContext* browser_context)
    : browser_context_(browser_context) {}

SerialChooserContext::~SerialChooserContext() = default;

void SerialChooserContext::GrantPortPermission(
    const url::Origin& origin,
    const device::mojom::SerialPortInfo& port) {
  base::Value value = PortInfoToValue(port);
  port_info_.insert({port.token, value.Clone()});

  if (CanStorePersistentEntry(port)) {
    browser_context_->GrantObjectPermission(origin, std::move(value),
                                            kSerialGrantedDevicesPref);
    return;
  }

  ephemeral_ports_[origin].insert(port.token);
}

bool SerialChooserContext::HasPortPermission(
    const url::Origin& origin,
    const device::mojom::SerialPortInfo& port) {
  auto it = ephemeral_ports_.find(origin);
  if (it != ephemeral_ports_.end()) {
    const std::set<base::UnguessableToken> ports = it->second;
    if (base::Contains(ports, port.token))
      return true;
  }

  if (!CanStorePersistentEntry(port)) {
    return false;
  }

  std::vector<std::unique_ptr<base::Value>> object_list =
      browser_context_->GetGrantedObjects(origin, kSerialGrantedDevicesPref);
  for (const auto& device : object_list) {
#if defined(OS_WIN)
    const std::string& device_instance_id =
        *device->FindStringKey(kDeviceInstanceIdKey);
    if (port.device_instance_id == device_instance_id)
      return true;
#else
    const int vendor_id = *device->FindIntKey(kVendorIdKey);
    const int product_id = *device->FindIntKey(kProductIdKey);
    const std::string& serial_number = *device->FindStringKey(kSerialNumberKey);

    // Guaranteed by the CanStorePersistentEntry) check above.
    DCHECK(port.has_vendor_id);
    DCHECK(port.has_product_id);
    DCHECK(port.serial_number && !port.serial_number->empty());
    if (port.vendor_id != vendor_id || port.product_id != product_id ||
        port.serial_number != serial_number) {
      continue;
    }

#if defined(OS_MAC)
    const std::string& usb_driver_name = *device->FindStringKey(kUsbDriverKey);
    if (port.usb_driver_name != usb_driver_name) {
      continue;
    }
#endif  // defined(OS_MAC)

    return true;
#endif  // defined(OS_WIN)
  }
  return false;
}

// static
bool SerialChooserContext::CanStorePersistentEntry(
    const device::mojom::SerialPortInfo& port) {
  // If there is no display name then the path name will be used instead. The
  // path name is not guaranteed to be stable. For example, on Linux the name
  // "ttyUSB0" is reused for any USB serial device. A name like that would be
  // confusing to show in settings when the device is disconnected.
  if (!port.display_name || port.display_name->empty())
    return false;

#if defined(OS_WIN)
  return !port.device_instance_id.empty();
#else
  if (!port.has_vendor_id || !port.has_product_id || !port.serial_number ||
      port.serial_number->empty()) {
    return false;
  }

#if defined(OS_MAC)
  // The combination of the standard USB vendor ID, product ID and serial
  // number properties should be enough to uniquely identify a device
  // however recent versions of macOS include built-in drivers for common
  // types of USB-to-serial adapters while their manufacturers still
  // recommend installing their custom drivers. When both are loaded two
  // IOSerialBSDClient instances are found for each device. Including the
  // USB driver name allows us to distinguish between the two.
  if (!port.usb_driver_name || port.usb_driver_name->empty())
    return false;
#endif  // defined(OS_MAC)

  return true;
#endif  // defined(OS_WIN)
}

device::mojom::SerialPortManager* SerialChooserContext::GetPortManager() {
  EnsurePortManagerConnection();
  return port_manager_.get();
}

void SerialChooserContext::AddPortObserver(PortObserver* observer) {
  port_observer_list_.AddObserver(observer);
}

void SerialChooserContext::RemovePortObserver(PortObserver* observer) {
  port_observer_list_.RemoveObserver(observer);
}

base::WeakPtr<SerialChooserContext> SerialChooserContext::AsWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void SerialChooserContext::OnPortAdded(device::mojom::SerialPortInfoPtr port) {
  for (auto& observer : port_observer_list_)
    observer.OnPortAdded(*port);
}

void SerialChooserContext::OnPortRemoved(
    device::mojom::SerialPortInfoPtr port) {
  for (auto& observer : port_observer_list_)
    observer.OnPortRemoved(*port);

  port_info_.erase(port->token);
}

void SerialChooserContext::EnsurePortManagerConnection() {
  if (port_manager_)
    return;

  mojo::PendingRemote<device::mojom::SerialPortManager> manager;
  content::GetDeviceService().BindSerialPortManager(
      manager.InitWithNewPipeAndPassReceiver());
  SetUpPortManagerConnection(std::move(manager));
}

void SerialChooserContext::SetUpPortManagerConnection(
    mojo::PendingRemote<device::mojom::SerialPortManager> manager) {
  port_manager_.Bind(std::move(manager));
  port_manager_.set_disconnect_handler(
      base::BindOnce(&SerialChooserContext::OnPortManagerConnectionError,
                     base::Unretained(this)));

  port_manager_->SetClient(client_receiver_.BindNewPipeAndPassRemote());
}

void SerialChooserContext::OnPortManagerConnectionError() {
  port_manager_.reset();
  client_receiver_.reset();

  port_info_.clear();

  ephemeral_ports_.clear();
}

}  // namespace electron