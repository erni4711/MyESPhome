# `/admin` API

HTTP API exposed by the local web administrator. Unless stated otherwise, all
endpoints return JSON and use the `/admin` prefix.

## Authentication

The API uses the same authentication as the admin web interface. Send the
session cookie or other authentication credentials issued by the web server.
Unauthenticated requests return `401 Unauthorized`.

## Response format

Successful responses use a `2xx` status code. Errors use this format:

```json
{
	"error": "A human-readable error message"
}
```

Common status codes:

| Status | Meaning |
| --- | --- |
| `200` | Request completed successfully |
| `400` | Invalid request or JSON payload |
| `401` | Authentication required |
| `404` | Resource not found |
| `409` | Request conflicts with the current state |
| `500` | Server error |

## Endpoints

### `GET /admin`

Returns the admin web application. API clients should use the JSON endpoints
below rather than parsing this response.

### `GET /admin/status`

Returns the current device and service status.

Example response:

```json
{
	"online": true,
	"uptime": 86400,
	"version": "1.0.0"
}
```

### `GET /admin/config`

Returns the current admin configuration as JSON.

### `PUT /admin/config`

Updates the admin configuration. The request body must be a JSON object. The
server validates the complete configuration before applying it.

```http
Content-Type: application/json
```

```json
{
	"key": "value"
}
```

Returns the saved configuration.

### `GET /admin/logs`

Returns recent administrator logs. An optional `limit` query parameter limits
the number of entries returned, for example:

```text
GET /admin/logs?limit=100
```

### `GET /admin/tiles`

Returns the configured tiles as JSON.

### `PUT /admin/tiles`

Replaces the configured tiles. The request body must be a JSON array. The
server validates the complete tile list before applying it.

### `GET /admin/folders`

Returns the configured folders as JSON.

### `PUT /admin/folders`

Replaces the configured folders. The request body must be a JSON array. The
server validates the complete folder list before applying it.

### Consumable HTML URI

The admin web application can be consumed as HTML at:

```text
/admin
```

This URI returns the admin HTML document. Clients that need structured data
should use the JSON endpoints above.

### `POST /admin/restart`

Requests a service restart. The request body is empty. A successful request
returns `202 Accepted`:

```json
{
	"accepted": true
}
```

## Example

```bash
curl -b cookies.txt http://device.local/admin/api/status
```

All state-changing requests should include `Content-Type: application/json`
and must be authenticated.

