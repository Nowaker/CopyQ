// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QVariantMap>

class QByteArray;
class QString;

/**
 * Label for an item without text, the same as Flycut writes on macOS, for example
 * "[image: PNG, 1920×1080, 1.2 MB]", "[PDF: 3 pages, 120 KB]", "[file: notes.txt]"
 * or "[object: application/x-foo, 12 bytes]".
 *
 * The item's text is not consulted: callers show the label only for items without text.
 * For an item whose binary formats were evicted (see mimeEvictedFormats), the label
 * recorded at eviction is returned. Empty if the item has no user-visible format.
 */
QString itemLabel(const QVariantMap &data);

/// Byte count as macOS writes file sizes (1000-based): "12 bytes", "2 KB", "1.2 MB".
QString byteCountLabel(qint64 bytes);
