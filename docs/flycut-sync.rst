Sync with Flycut
================

CopyQ can share its clipboard history and favorites with Flycut on macOS
through `flycut-sync-server`, as one more machine next to the Macs.

What is shared
--------------

- The clipboard tab is Flycut's history; the tab set in **Favorites tab**
  (default ``Favorites``) is Flycut's favorites. Both keep Flycut's order.
- A new copy, a copy made again, an item moved or edited is shared. Every
  format of an item travels (HTML, images, an app's own formats), and
  machine-specific types are mapped to macOS pasteboard types and back.
- Deleting an item shares the deletion. Items dropped by the tab's item limit,
  expiry or the storage budget are not deleted elsewhere.
- Items copied on other machines are inserted where their rank puts them;
  items deleted or cleared there are removed here.
- Credentials (the rules Flycut uses) are sealed with keys derived from the
  server token before they leave this machine, text and every format.
- Formats evicted to save space are fetched from the server again when the
  item is copied or pasted.

Configuration
-------------

1. Open "Preferences" (``Ctrl+P`` shortcut) and go to "Sync".
2. Enter the server address, e.g. ``https://flycut.example.ts.net``, and the
   server token (the contents of its token file). The token is stored in the
   system keychain, never in the configuration file.
3. Check **Sync history and favorites with Flycut** and click OK.

The page shows the sync's status. Options ``flycut_sync``,
``flycut_sync_server`` and ``flycut_sync_favorites_tab`` can also be set with
the ``config()`` command; ``flycut_sync_token_file`` reads the token from a
file instead of the keychain.

On its first start CopyQ takes over the state of `flycut-copyq-bridge`
(option ``flycut_sync_bridge_state``, default
``~/.local/share/flycut-copyq-bridge``): its machine name, read positions, log
and pasteboard data, so the history on the server continues and nothing is
uploaded or inserted twice. Stop the bridge before enabling the sync.

The sync's own state is kept in ``flycut-sync`` in the item data directory.

Archive search
--------------

When the sync is on, searching in the search popup (:js:func:`searchPopup`,
global shortcut command "Search items in a popup") lists the local matches
first and ends with **Search full archive on server…**. Choosing it searches
the whole server history of the tab's list; clippings found there that this
machine doesn't hold are listed below it, marked "on server". Choosing one adds
it to the clipboard tab as a new copy, with all its formats, and pastes it.

Web history
-----------

**Open History in Browser** (in the File menu and the tray menu) opens the
server's web history in the default browser, signed in with a one-time link.

Scripting
---------

See :js:func:`syncStatus`, :js:func:`syncNow`,
:js:func:`openHistoryInBrowser`, :js:func:`syncHistoryLink` and
:js:func:`searchSyncArchive`.
