# Vendored protobuf header

`plugin.cpp` includes `eiface.h` for `IVEngineServer2::ServerCommand`, and `eiface.h` includes
`network_connection.pb.h`. That is the only protobuf this plugin touches, and it touches it only by
including it — no type from it is ever used, so nothing here is compiled or linked. Its one
`.pb.h` dependency, `google/protobuf/descriptor.pb.h`, ships in the SDK.

Vendored rather than generated so the build needs neither protoc nor a protobuf library. Regenerate
after an SDK update with protoc 21.8, matching `hl2sdk-cs2/thirdparty/protobuf-3.21.8`:

```
protoc -I. -I<protoc>/include --cpp_out=. network_connection.proto
```

`network_connection.proto` is copied from `hl2sdk-cs2/common/`.
