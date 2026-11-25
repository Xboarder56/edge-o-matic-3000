# WebSocket Communication

The Edge-o-Matic 3000 serves as a WebSocket host for streaming data over wireless networks. The NoGasm UI project makes 
use of this, but you are free to develop your own interface.

The WebSocket interface uses JSON serialization for both sending and receiving packets. The structure of a payload sent 
TO the Edge-o-Matic is a key-value object with each key corresponding to a command, and a value corresponding to the 
arguments to that command. Responses streamed from the device should be handled similarly. 

**An example request sent to the WebSocket channel:**

```json
{
    "configSet": {
        "motor_max_speed": 255
    },
    "configList": {
        "nonce": 1234
    }
}
```
 
**The associated response:**

```json
{
    "configList": {
        "nonce": 1234,
        "config": {…}
    }
}
```

Multiple commands can be sent in either direction, but only one of each command type can be specified. It is recommended
that you send only one command per request. To track responses to commands, you can use the “nonce” parameter, which 
some commands support. This is a numeric identifier that is returned with the associated response and can be used to 
filter duplicate responses.


## Server Commands

These commands are recognized by the WebSocket server running on the Edge-o-Matic 3000:
 

### `configSet`
Sets one or more configuration values.

**Arguments:**

|Argument|Type|Description|
|---|---|---|
|*|\<any>|Config keys / values|

**Example:**
```json
"configSet": { 
    "motor_max_speed": 255 
}
```
 

### `configList`
Requests all config values to be sent.

**Example:**
```json
"configList": null
```
 

### `serialCmd`	
Execute a string as a serial command.

**Arguments:**

|Argument|Type|Description|
|---|---|---|
|cmd|String|Command to execute|
|nonce|Numeric|Returned in response|

**Example:**
```json
"serialCmd": {
    "cmd": "external enable",
    "nonce": 1234
}
```
 

### `getWiFiStatus`
Requests the Wi-Fi status to be sent.

**Example:**
```json
"getWiFiStatus": {}
```
 

### `getSDStatus`
Requests SD Card status to be sent.

**Example:**
```json
"getSDStatus": {}
```
 

### `setMode`
Sets the current run mode.

**Arguments:**

|Argument|Type|Description|
|---|---|---|
|mode|String|`automatic`, `manual`|

**Example:**
```json
"setMode": "automatic"
```
 

### `setMotor`
Sets vibration speed. Will enter manual mode.

**Arguments:**

|Argument|Type|Description|
|---|---|---|
|speed|Byte|0-255 motor speed|

**Example:**
```json
"setMotor": 128
```
 

### `streamReadings`
Subscribes to (`true`) or stops (`false`) the `readings` broadcast. Subscribing also subscribes
to system status (Wi-Fi, SD). Before 2.1.0-rc.3, any value subscribed.

**Example:**
```json
"streamReadings": false
```
 

### `streamEvents`
Since 2.1.0-rc.3. Subscribes to (`true`) or stops (`false`) device events, delivered as `event`
messages as they happen. Clients that do not subscribe receive nothing new.

**Example:**
```json
"streamEvents": true
```
 

### `getState`
Since 2.1.0-rc.5. Returns a snapshot of the device state, so a client can resync right after
connecting without waiting for `readings`. Supports `nonce`.

|Field|Type|Description|
|---|---|---|
|runMode|String|`MANUAL_CONTROL`, `AUTOMAITC_CONTROL` (sic) or `PLUGIN_CONTROL`|
|mode|Numeric|0 manual, 1 automatic, 2 plugin|
|motor|Numeric|Current motor speed (0-255)|
|motorMax|Numeric|Configured maximum motor speed|
|arousal|Numeric|Current arousal value|
|threshold|Numeric|Arousal threshold (`sensitivity_threshold`)|
|pressure|Numeric|Last pressure reading|
|denials|Numeric|Denials since boot or the last reset|
|pluginControl|Boolean|A plugin holds motor control|
|lock|Object|`{"locked": bool, "reason": string}`: the button/encoder lock|
|detection|Object|`{"armed": bool, "state": string}`: orgasm detection|
|wifiOn, btOn|Boolean|Configured radio state|
|freeHeap|Numeric|Free internal heap, bytes|
|fwVersion|String|Firmware version|
|millis|Numeric|Millisecond timestamp|

**Example:**
```json
"getState": {"nonce": 7}
```

### `resetDenials`
Since 2.1.0-rc.5. Resets the denial counter, like "Reset Denial Count" on the device. Responds
with `{"denials": 0}`.
```json
"resetDenials": {}
```

### `setLock`
Since 2.1.0-rc.5. Locks or unlocks the device buttons and encoder. Holding BACK+OK for 5 seconds
on the device always unlocks. Responds with the resulting `locked` and `reason`; subscribers to
`streamEvents` get a `lock_changed` event.

|Argument|Type|Description|
|---|---|---|
|locked|Boolean|Required|
|reason|String|Optional, shown with the lock (default `Remote lock`)|

```json
"setLock": {"locked": true, "reason": "Session running"}
```

### `pluginList`
Since 2.1.0-rc.5. Lists loaded plugins: `{"plugins": [{"name", "displayName", "type",
"description"}]}`. `type` is `feature` or `ble_driver`.
```json
"pluginList": {}
```

### `pluginEvent`
Since 2.1.0-rc.5. Runs a feature plugin's `remote_<event>` handler. Plugins choose what can be
triggered by naming handlers `remote_*` (see `doc/PluginHostAPI.md`).

|Argument|Type|Description|
|---|---|---|
|plugin|String|Plugin name, as in `pluginList`|
|event|String|Handler name without the `remote_` prefix: lowercase letters, digits, `_`, up to 24 characters|
|value|Numeric|Optional argument passed to the handler (default 0)|

The response has `handled: true` and the handler's `status`, or `errno: 1` with an `error`:
`plugin not loaded`, `plugin has no handler for this event`, or `driver plugins cannot take
remote events`.
```json
"pluginEvent": {"plugin": "smart-lock", "event": "release", "value": 0}
```

### `getDevices`
Since 2.1.0-rc.5. Bluetooth devices currently bound to a driver plugin:
`{"devices": [{"name": "LVS-Hush", "plugin": "lovense-driver"}]}`.
```json
"getDevices": {}
```

## Server Responses
Your application should be prepared to handle these messages streamed from the server. The actual data may change as 
this is a printed document and not live documentation. See GitHub for more up-to-date details.


### `configList`
A listing of the current configuration.

**Parameters:**

|Parameter|Type|Description|
|---|---|---|
|*|<any>|Current serialized configuration|

**Example:**
```json
"configList": {
    "motor_max_speed": 255,
    "wifi_on": false
}
```
 

### `serialCmd`
The response from a serial command.

**Parameters:**

|Parameter|Type|Description|
|---|---|---|
|text|String|Output text from command|
|nonce|Numeric|Same as initial request|

**Example:**
```json
"serialCmd": {
    "nonce": 1234,
    "text": "Enabled external bus\nOK\n”
}
```
 

### `wifiStatus`
The current Wi-Fi connection status.

**Parameters:**

|Parameter|Type|Description|
|---|---|---|
|ssid|String|The connected network SSID|
|ip|String|Device IP Address|
|rssi|Numeric|RSSI Value (signal strength)|

**Example:**
```json
"wifiStatus": {
    "rssi": -56,
    "ssid": "FBI Spy-Fi",
    "ip": "10.0.102.192"
}
```
 

### `sdStatus`
Current SD card status.

**Parameters:**

|Parameter|Type|Description|
|---|---|---|
|size|Numeric|Size of card in MB|
|type|String|Type of card inserted|

**Example:**
```json
"sdStatus": {
    "size": 127,
    "type": "MMC"
}
```
 

### `readings`
A collection of current readings and device status. This is streamed at the global update frequency, unless disabled, 
and is used for providing real-time updates to your application.

**Parameters:**

|Parameter|Type|Description|
|---|---|---|
|pressure|Numeric|Current pressure reading|
|pavg|Numeric|Rolling pressure average|
|motor|Numeric|Current vibrator speed|
|arousal|Numeric|Current arousal value|
|denials|Numeric|Denials since boot or "Reset Denial Count", as on the device screen (2.1.0-rc.3+)|
|millis|Numeric|Millisecond timestamp|

**Example:**
```json
"readings": {
    "pressure": 1029,
    "pavg": 1028,
    "motor": 255,
    "arousal": 10,
    "millis": 198452
}
```


 

### `event`
Since 2.1.0-rc.3, sent to clients subscribed with `streamEvents`. Event names match the plugin
event names. `edge_start` (since 2.1.0-rc.5) fires when orgasm detection starts tracking a
sustained clench; it and `orgasm_start` are only sent while orgasm detection is armed.

**Parameters:**

|Parameter|Type|Description|
|---|---|---|
|name|String|`edge_start`, `orgasm_denial`, `orgasm_permit`, `orgasm_start`, `mode_set`, `lock_changed`, `config_changed`, `device_connect` or `device_disconnect`|
|value|Numeric|For `mode_set`, the new mode number (0 manual, 1 automatic, 2 plugin). For `lock_changed` and `device_*`, 1 or 0. For `config_changed`, 0 (send `configList` to read the new settings). For the arousal events, the arousal level when the event fired (before 2.1.0-rc.5 this was always 0).|
|millis|Numeric|Millisecond timestamp|
|runMode|String|`mode_set` only: the new run mode (e.g. `MANUAL_CONTROL`)|
|locked, reason|Boolean, String|`lock_changed` only (2.1.0-rc.5+)|
|device, plugin|String|`device_connect` / `device_disconnect` only: device name and driver plugin (2.1.0-rc.5+)|

**Example:**
```json
"event": {
    "name": "orgasm_denial",
    "value": 612,
    "millis": 198452
}
```
