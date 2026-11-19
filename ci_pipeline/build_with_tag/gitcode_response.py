"""Bounded reads for GitCode API and upload responses."""

MAX_JSON_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_ERROR_RESPONSE_BYTES = 4 * 1024


def read_json_response_body(response, request_label):
    payload = response.read(MAX_JSON_RESPONSE_BYTES + 1)
    if len(payload) > MAX_JSON_RESPONSE_BYTES:
        raise RuntimeError(
            f"{request_label}: response exceeds {MAX_JSON_RESPONSE_BYTES} bytes"
        )
    return payload.decode("utf-8", "replace")


def read_error_excerpt(response, max_chars):
    return response.read(MAX_ERROR_RESPONSE_BYTES).decode("utf-8", "replace")[
        :max_chars
    ]
