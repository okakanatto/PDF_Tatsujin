"""Small helpers shared by frozen fixture generators."""

from pypdf.generic import DecodedStreamObject, NameObject


def stream(data, **entries):
    value = DecodedStreamObject()
    value.set_data(data)
    for key, entry in entries.items():
        value[NameObject("/" + key)] = entry
    return value
