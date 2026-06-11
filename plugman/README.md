# Double Commander Plugin Manager (plugman)

Here is the architectural and functional specification overview for the standalone Double Commander Plugin Manager.

## Core Plugin Operations

* **Install:** Operates as an automated deployment pipeline. It extracts a downloaded archive to a temporary location, scans for recognized plugin binaries (`.wcx`, `.wlx.so`, etc.), copies the files to the designated Double Commander plugin directory, and appends a newly formed XML node to the appropriate section of `doublecmd.xml`.
* **Uninstall:** Executes a two-step destructive action. It parses `doublecmd.xml` to delete the target plugin's node from the XML tree, then deletes the physical directory containing the plugin binaries and its associated configuration files.
* **Disable / Enable:** Executes a non-destructive state change. To disable a plugin, the manager extracts the target XML node and relocates it to a custom XML node ignored by Double Commander, such as `<DisabledPlugins>`. To enable it, the node is moved back to its original active category. The physical files on the disk remain untouched.
* **Ordering:** Changes the index position of the XML node within its parent element before saving the file. This dictates the physical sequence of the XML child nodes, which determines the execution priority for Lister and Content plugins handling identical file extensions.

---

## Configuration and Tuning

* **Tweak (Packer Plugins):** Modifies API-level behaviors by reading the `<Flags>` bitmask integer in the XML. It decodes this integer into UI checkboxes using a bitwise `and` operation, and recalculates a new integer based on user input using a bitwise `or` operation.
* **Tweak (Lister Plugins):** Provides string manipulation for the `<DetectString>` node (e.g., `EXT="TXT"`), dictating exactly which file types trigger the plugin.
* **Configure:** Omitted to ensure stability. Dynamically loading shared libraries to hook into undocumented GUI functions risks segmentation faults. Users modifying internal plugin configurations will need to open the plugin's specific `.ini` or `.conf` file directly in their native text editor, such as `tilde`.

---

## Process and State Management

* **Restart Sequence:** Prevents file locking and data corruption using Free Pascal's `TProcess`. The manager sends a graceful termination signal (`SIGTERM` or `WM_CLOSE`), polls the operating system until the Double Commander process ID is completely released, executes the `doublecmd.xml` write operations, and relaunches the executable.

---

## Update and Rollback Engine

* **URL Binding:** Allows the user to link a direct download URL to a specific installed plugin. The manager records the remote URL alongside the local plugin's current file size and HTTP header metadata.
* **HEAD Polling:** Sends an HTTP `HEAD` request to the linked URL to evaluate `Content-Length`, `ETag`, or `Last-Modified` headers. This determines if the remote file has changed without utilizing the bandwidth required to download the full archive.
* **Server Compatibility Warning:** Detects if a remote server rejects `HEAD` requests or fails to provide validation headers, warning the user that automated version checking is unsupported for that specific link.
* **Update Disclaimer:** Triggers a strict prompt when a file change is detected, requiring user consent to proceed. The prompt explicitly states: "The remote file has changed. It cannot be verified if this is a newer version or a working build."
* **Backup and Rollback:** Prior to applying an update, the manager renames the existing plugin binary (e.g., changing `plugin.wlx.so` to `plugin.wlx.so.bkp`). A rollback function in the UI allows the user to restore the `.bkp` file to its original state if the updated plugin fails to load.

---

## Technology Stack

* **Framework:** Built entirely in Lazarus (Free Pascal).
* **Widgetset:** Targets Qt6 via the `libQt6Pas` binding library for native, cross-platform UI rendering.
* **Data Parsing:** Utilizes the Free Component Library (FCL-XML) to load `doublecmd.xml` into a `TXMLDocument` for strict DOM manipulation, avoiding regex entirely.

Because Double Commander is prone to overwriting its own XML file and dropping unrecognized nodes during a configuration flush, the plugman data will have to be stored in a separate file. It will be JSON.

Here is the structured JSON schema designed to support the installation, update checking, and rollback pipelines.

This structure uses nested objects to separate core identity, network capabilities, version tracking, and rollback states, making it easier to parse into native Pascal objects using the `fpjson` unit.

### The JSON Schema (`plugman.json`)

```json
{
  "schema_version": 1,
  "last_updated": "2026-06-10T23:58:28Z",
  "plugins": [
    {
      "id": "123e4567-e89b-12d3-a456-426614174000",
      "name": "AudioInfo",
      "type": "wdx",
      "filename": "audioinfo.wdx",
      "relative_dir": "wdx/audioinfo/",
      "source": {
        "url": "https://example.com/audioinfo.zip",
        "http_capabilities": {
          "supports_head": true,
          "provides_etag": true,
          "provides_modified": false
        }
      },
      "state_tracking": {
        "etag": "\"5d8c72a5-12a\"",
        "last_modified": "Wed, 01 May 2026 12:00:00 GMT",
        "local_size": 1250000,
        "last_checked": "2026-06-10T23:58:28Z"
      },
      "rollback": {
        "backup_exists": true,
        "backup_filename": "audioinfo.wdx.bkp",
        "backup_timestamp": "2026-05-01T10:00:00Z"
      }
    }
  ]
}
```

### Schema Node Definitions

**Root Level**

* `schema_version`: An integer. Essential for future-proofing your application. If you add new features later that require structural changes to this file, your Lazarus code can read this integer and run a migration function on startup.
* `last_updated`: Global timestamp of the last write operation to this file.

**Plugin Identity Data**

* `id`: A generated GUID. This ensures the manager does not lose track of the plugin's metadata if the user renames the directory or the file itself.
* `type`: Identifies the category (`wlx`, `wcx`, `wfx`, `wdx`), simplifying the logic when you need to match this entry against `doublecmd.xml`.
* `relative_dir`: The path relative to Double Commander's base plugin directory (e.g., `~/.config/doublecmd/plugins/`). This avoids hardcoding absolute OS paths, maintaining compatibility if the user moves their installation.

**Source Data**

* `url`: The direct download link provided by the user.
* `http_capabilities`: This object caches the server's behavior upon the first successful installation or check.
* If `supports_head` is written as `false`, your code knows to immediately fall back to a `GET` request and full download hash check on all future update attempts, bypassing a guaranteed `HEAD` failure.

**State Tracking (The Update Engine)**

* `etag` and `last_modified`: Stored exactly as received from the HTTP headers during the last successful install or update. These are the primary keys used for comparison during the next update check.
* `local_size`: The exact byte size of the `.zip` or binary downloaded. Used as a secondary verification metric, or a primary metric if the server lacks `ETag` and `Last-Modified` support.
* `last_checked`: Records the last time the manager pinged the server.

**Rollback Engine**

* `backup_exists`: A boolean flag that dictates whether the "Rollback" button in your UI is enabled or disabled for this specific plugin.
* `backup_filename`: The exact name of the archived file.
* `backup_timestamp`: Determines the age of the backup, allowing you to display information such as "Restore previous version (Backed up on May 1st)" in the UI.
