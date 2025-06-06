<?php
/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
class SecureEndToEndTest extends \PHPUnit\Framework\TestCase
{
    private $server;
    private $port;
    private $host_override;
    private $channel;

    public function setUp(): void
    {
        $credentials = Grpc\ChannelCredentials::createSsl(
            file_get_contents(dirname(__FILE__).'/../data/ca.pem'));
        $server_credentials = Grpc\ServerCredentials::createSsl(
            null,
            file_get_contents(dirname(__FILE__).'/../data/server1.key'),
            file_get_contents(dirname(__FILE__).'/../data/server1.pem'));
        $this->server = new Grpc\Server();
        $this->port = $this->server->addSecureHttp2Port('0.0.0.0:0',
                                              $server_credentials);
        $this->server->start();
        $this->host_override = 'foo.test.google.fr';
        $this->channel = new Grpc\Channel(
            'localhost:'.$this->port,
            [
            'force_new' => true,
            'grpc.ssl_target_name_override' => $this->host_override,
            'grpc.default_authority' => $this->host_override,
            'credentials' => $credentials,
            ]
        );
    }

    public function tearDown(): void
    {
        $this->channel->close();
        unset($this->server);
    }

    public function testSimpleRequestBody()
    {
        $deadline = Grpc\Timeval::infFuture();
        $status_text = 'xyz';
        $call = new Grpc\Call($this->channel,
                              'phony_method',
                              $deadline,
                              $this->host_override);

        $event = $call->startBatch([
            Grpc\OP_SEND_INITIAL_METADATA => [],
            Grpc\OP_SEND_CLOSE_FROM_CLIENT => true,
        ]);

        $this->assertTrue($event->send_metadata);
        $this->assertTrue($event->send_close);

        $event = $this->server->requestCall();
        $this->assertSame('phony_method', $event->method);
        $server_call = $event->call;

        $event = $server_call->startBatch([
            Grpc\OP_SEND_INITIAL_METADATA => [],
            Grpc\OP_SEND_STATUS_FROM_SERVER => [
                'metadata' => [],
                'code' => Grpc\STATUS_INVALID_ARGUMENT,
                'details' => $status_text,
            ],
            Grpc\OP_RECV_CLOSE_ON_SERVER => true,
        ]);

        $this->assertTrue($event->send_metadata);
        $this->assertTrue($event->send_status);
        $this->assertFalse($event->cancelled);

        $event = $call->startBatch([
            Grpc\OP_RECV_INITIAL_METADATA => true,
            Grpc\OP_RECV_STATUS_ON_CLIENT => true,
        ]);

        $this->assertSame([], $event->metadata);
        $status = $event->status;
        $this->assertSame([], $status->metadata);
        $this->assertSame(Grpc\STATUS_INVALID_ARGUMENT, $status->code);
        $this->assertSame($status_text, $status->details);

        unset($call);
        unset($server_call);
    }

    public function testMessageWriteFlags()
    {
        $deadline = Grpc\Timeval::infFuture();
        $req_text = 'message_write_flags_test';
        $call = new Grpc\Call($this->channel,
                              'phony_method',
                              $deadline,
                              $this->host_override);

        $event = $call->startBatch([
            Grpc\OP_SEND_INITIAL_METADATA => [],
            Grpc\OP_SEND_MESSAGE => ['message' => $req_text,
                                     'flags' => Grpc\WRITE_NO_COMPRESS, ],
            Grpc\OP_SEND_CLOSE_FROM_CLIENT => true,
        ]);

        $this->assertTrue($event->send_metadata);
        $this->assertTrue($event->send_close);

        $event = $this->server->requestCall();
        $this->assertSame('phony_method', $event->method);
        $server_call = $event->call;

        $event = $server_call->startBatch([
            Grpc\OP_SEND_INITIAL_METADATA => [],
            Grpc\OP_SEND_STATUS_FROM_SERVER => [
                'metadata' => [],
                'code' => Grpc\STATUS_OK,
                'details' => '',
            ],
        ]);

        $event = $call->startBatch([
            Grpc\OP_RECV_INITIAL_METADATA => true,
            Grpc\OP_RECV_STATUS_ON_CLIENT => true,
        ]);

        $this->assertSame([], $event->metadata);
        $status = $event->status;
        $this->assertSame([], $status->metadata);
        $this->assertSame(Grpc\STATUS_OK, $status->code);
        $this->assertSame("", $status->details);

        unset($call);
        unset($server_call);
    }

    public function testClientServerFullRequestResponse()
    {
        $deadline = Grpc\Timeval::infFuture();
        $req_text = 'client_server_full_request_response';
        $reply_text = 'reply:client_server_full_request_response';

        $call = new Grpc\Call($this->channel,
                              'phony_method',
                              $deadline,
                              $this->host_override);

        $event = $call->startBatch([
            Grpc\OP_SEND_INITIAL_METADATA => [],
            Grpc\OP_SEND_CLOSE_FROM_CLIENT => true,
            Grpc\OP_SEND_MESSAGE => ['message' => $req_text],
        ]);

        $this->assertTrue($event->send_metadata);
        $this->assertTrue($event->send_close);
        $this->assertTrue($event->send_message);

        $event = $this->server->requestCall();
        $this->assertSame('phony_method', $event->method);
        $server_call = $event->call;

        $event = $server_call->startBatch([
            Grpc\OP_RECV_MESSAGE => true,
        ]);

        $this->assertSame($req_text, $event->message);

        $event = $server_call->startBatch([
            Grpc\OP_SEND_INITIAL_METADATA => [],
            Grpc\OP_SEND_MESSAGE => ['message' => $reply_text],
            Grpc\OP_SEND_STATUS_FROM_SERVER => [
                'metadata' => [],
                'code' => Grpc\STATUS_OK,
                'details' => '',
            ],
            Grpc\OP_RECV_CLOSE_ON_SERVER => true,
        ]);

        $this->assertTrue($event->send_metadata);
        $this->assertTrue($event->send_status);
        $this->assertTrue($event->send_message);
        $this->assertFalse($event->cancelled);

        $event = $call->startBatch([
            Grpc\OP_RECV_INITIAL_METADATA => true,
            Grpc\OP_RECV_MESSAGE => true,
            Grpc\OP_RECV_STATUS_ON_CLIENT => true,
        ]);

        $this->assertSame([], $event->metadata);
        $this->assertSame($reply_text, $event->message);
        $status = $event->status;
        $this->assertSame([], $status->metadata);
        $this->assertSame(Grpc\STATUS_OK, $status->code);
        $this->assertSame("", $status->details);

        unset($call);
        unset($server_call);
    }
}
