from array import array

import pika


HOST = 'localhost'
QUEUE_NAME = 'hello_queue'

connection = pika.BlockingConnection(pika.ConnectionParameters(HOST))
channel = connection.channel()
channel.queue_declare(queue=QUEUE_NAME)

def read_value() -> float:
    pass

def main():
    data = array('f', range(100))
    
    channel.basic_publish(exchange='',
                        routing_key=QUEUE_NAME,
                        body=message)
    connection.close()

    print(f'[x] Sent {message} to queue {QUEUE_NAME}')