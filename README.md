# Asynchronous Web Server

- This application represents an asynchronous web server that sends files to clients using the HTTP protocol.
- The files can be of two types: static and dynamic.

- Each connection is characterized by a state at a given moment in the program. For example, at the beginning, the connection has the state `CONN_INIT`. After receiving the request from the client in complete format, the connection is in the state `CONN_WREADY`, indicating that the message is ready to be sent to the client.

- The reception of the request from the client is not instantaneous because the socket read function may not read the entire message. In this case, the connection remains in the `CONN_READING` state until the read event is signaled again in `epoll`, and the remaining bytes are read.

- The first step is sending the header, which contains the status code, message, protocol name, and other information such as the size and type of the message to be sent, etc.

- Currently, there are two possible codes that can be sent: 200 OK and 404 NOT FOUND.

## Sending Static Files

- Static files are sent directly using the `sendfile` function without the need for additional buffers. The connection is in the `CONN_SENDING` state until `sendfile` sends the complete number of bytes to the client.
- The connection is then closed, freeing the memory used by the connection structure.

## Sending Dynamic Files

- Dynamic files require a step where they are loaded into memory using a list of buffers that are populated with the actual data using asynchronous operations.
- While the file is asynchronously loaded into memory, the connection is in the `CONN_READING_ASYNC` state, allowing other connections to be completed.
- Once the entire file is loaded into memory, the buffers are sent to the client one by one using the `send` function. Since only a portion of the buffer can be sent at a time, the action continues when signaled in `epoll`.

## Note

- Both the header and the file buffers for dynamic files are sent using the `send_message` method, which sends the data in `conn.write_buff` to the connection's socket.
