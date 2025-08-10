#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/sys/printk.h>

/* Device tree nodes */
#define UART_NODE DT_CHOSEN(zephyr_console)

/* UART message structure for queue */
typedef struct {
    char data[64];
    size_t len;
    struct k_sem *completion_sem;  /* Optional - for synchronous sends */
    uint32_t sender_id;           /* For debugging priority inheritance */
} uart_msg_t;

/* Message queue for UART TX requests */
K_MSGQ_DEFINE(uart_tx_queue, sizeof(uart_msg_t), 10, 4);

/* Mutex for queue access protection (with priority inheritance) */
static struct k_mutex uart_queue_mutex;

/* Semaphores for UART operations */
static struct k_sem uart_tx_complete_sem;
static struct k_sem uart_rx_ready_sem;

/* UART device */
static const struct device *uart_dev;

/* Buffers for DMA operations */
static uint8_t rx_buffer[64];
static uint8_t rx_double_buffer[64];
static volatile bool use_rx_buffer_1 = true;

/* Statistics for monitoring priority inheritance */
static volatile uint32_t high_prio_msg_count = 0;
static volatile uint32_t low_prio_msg_count = 0;
static volatile uint32_t queue_contentions = 0;

/* Verify DMA configuration */
static void verify_dma_usage(void)
{
    printk("Checking DMA configuration...\n");
    
    #if DT_NODE_HAS_PROP(UART_NODE, dmas)
        printk("✓ UART has DMA configured in device tree\n");
    #else
        printk("⚠ UART does NOT have DMA in device tree\n");
    #endif
}

/* UART callback - handles DMA completion events */
static void uart_callback(const struct device *dev, struct uart_event *evt, void *user_data)
{
    switch (evt->type) {
    case UART_TX_DONE:
        printk("✓ DMA TX completed - %d bytes\n", evt->data.tx.len);
        k_sem_give(&uart_tx_complete_sem);
        break;
        
    case UART_TX_ABORTED:
        printk("✗ DMA TX aborted\n");
        k_sem_give(&uart_tx_complete_sem);
        break;
        
    case UART_RX_RDY:
        printk("✓ DMA RX ready - %d bytes\n", evt->data.rx.len);
        
        /* Process received data */
        uint8_t *data = &evt->data.rx.buf[evt->data.rx.offset];
        printk("RX via DMA: ");
        for (int i = 0; i < evt->data.rx.len; i++) {
            printk("%c", data[i]);
        }
        printk("\n");
        
        k_sem_give(&uart_rx_ready_sem);
        break;
        
    case UART_RX_BUF_REQUEST:
        printk("✓ DMA RX buffer request\n");
        /* Provide next buffer for continuous DMA */
        if (use_rx_buffer_1) {
            uart_rx_buf_rsp(uart_dev, rx_double_buffer, sizeof(rx_double_buffer));
            use_rx_buffer_1 = false;
        } else {
            uart_rx_buf_rsp(uart_dev, rx_buffer, sizeof(rx_buffer));
            use_rx_buffer_1 = true;
        }
        break;
        
    case UART_RX_BUF_RELEASED:
        printk("✓ DMA RX buffer released\n");
        break;
        
    case UART_RX_DISABLED:
        printk("DMA RX disabled\n");
        break;
        
    case UART_RX_STOPPED:
        printk("DMA RX stopped: %d\n", evt->data.rx_stop.reason);
        /* Restart RX */
        uart_rx_enable(uart_dev, rx_buffer, sizeof(rx_buffer), SYS_FOREVER_US);
        break;
        
    default:
        printk("UART event: %d\n", evt->type);
        break;
    }
}

/* Dedicated UART thread - handles all UART operations */
static void uart_worker_thread(void *p1, void *p2, void *p3)
{
    uart_msg_t msg;
    int ret;
    
    printk("UART worker thread started (handles all DMA operations)\n");
    
    while (1) {
        /* Wait for message from queue */
        ret = k_msgq_get(&uart_tx_queue, &msg, K_FOREVER);
        if (ret != 0) {
            printk("Failed to get message from queue: %d\n", ret);
            continue;
        }
        
        printk("[UART-WORKER] Processing message from sender %u (%d bytes)\n", 
               msg.sender_id, msg.len);
        
        /* Start DMA TX operation - only this thread accesses UART TX */
        ret = uart_tx(uart_dev, (uint8_t *)msg.data, msg.len, SYS_FOREVER_US);
        if (ret != 0) {
            printk("[UART-WORKER] DMA TX start failed: %d\n", ret);
            /* Signal completion even on failure */
            if (msg.completion_sem) {
                k_sem_give(msg.completion_sem);
            }
            continue;
        }
        
        /* Wait for DMA completion */
        ret = k_sem_take(&uart_tx_complete_sem, K_MSEC(5000));
        if (ret != 0) {
            printk("[UART-WORKER] DMA TX timeout\n");
        } else {
            printk("[UART-WORKER] ✓ DMA TX completed for sender %u\n", msg.sender_id);
        }
        
        /* Signal requesting thread if synchronous operation */
        if (msg.completion_sem) {
            k_sem_give(msg.completion_sem);
        }
    }
}

/* Queue a message for UART transmission with priority protection */
static int uart_send_queued(const char *data, size_t len, uint32_t sender_id, bool synchronous)
{
    uart_msg_t msg;
    struct k_sem completion_sem;
    int ret;
    
    if (len >= sizeof(msg.data)) {
        return -EINVAL;
    }
    
    /* Prepare message */
    memcpy(msg.data, data, len);
    msg.len = len;
    msg.sender_id = sender_id;
    
    if (synchronous) {
        k_sem_init(&completion_sem, 0, 1);
        msg.completion_sem = &completion_sem;
    } else {
        msg.completion_sem = NULL;
    }
    
    /* 
     * CRITICAL SECTION: Queue access protected by mutex with priority inheritance
     * This prevents priority inversion during queue operations
     */
    printk("[SENDER-%u] Requesting queue access...\n", sender_id);
    ret = k_mutex_lock(&uart_queue_mutex, K_MSEC(2000));
    if (ret != 0) {
        printk("[SENDER-%u] ✗ Failed to acquire queue mutex: %d\n", sender_id, ret);
        queue_contentions++;
        return ret;
    }
    
    printk("[SENDER-%u] ✓ Queue mutex acquired\n", sender_id);
    
    /* Put message in queue */
    ret = k_msgq_put(&uart_tx_queue, &msg, K_MSEC(1000));
    if (ret != 0) {
        printk("[SENDER-%u] ✗ Queue full: %d\n", sender_id, ret);
        k_mutex_unlock(&uart_queue_mutex);
        return ret;
    }
    
    printk("[SENDER-%u] ✓ Message queued successfully\n", sender_id);
    k_mutex_unlock(&uart_queue_mutex);
    
    /* Wait for completion if synchronous */
    if (synchronous) {
        printk("[SENDER-%u] Waiting for transmission completion...\n", sender_id);
        ret = k_sem_take(&completion_sem, K_MSEC(10000));
        if (ret != 0) {
            printk("[SENDER-%u] ✗ Completion timeout\n", sender_id);
            return ret;
        }
        printk("[SENDER-%u] ✓ Transmission completed\n", sender_id);
    }
    
    return 0;
}

/* High priority thread */
static void high_priority_task(void *p1, void *p2, void *p3)
{
    char msg[50];
    int ret;
    
    printk("[HIGH-PRIO] Thread started (Priority 5 - Cooperative)\n");
    
    while (1) {
        high_prio_msg_count++;
        snprintf(msg, sizeof(msg), "HIGH-PRIO MSG #%u\r\n", high_prio_msg_count);
        
        printk("[HIGH-PRIO] Sending message #%u...\n", high_prio_msg_count);
        
        /* Send synchronously to demonstrate priority inheritance */
        ret = uart_send_queued(msg, strlen(msg), 1, true);
        if (ret == 0) {
            printk("[HIGH-PRIO] ✓ Message sent successfully\n");
        } else {
            printk("[HIGH-PRIO] ✗ Message failed: %d\n", ret);
        }
        
        k_sleep(K_SECONDS(2));
    }
}

/* Medium priority thread - CPU intensive work (priority inversion test) */
static void medium_priority_task(void *p1, void *p2, void *p3)
{
    uint32_t work_count = 0;
    
    printk("[MEDIUM-PRIO] Thread started (Priority 10 - CPU intensive)\n");
    
    while (1) {
        printk("[MEDIUM-PRIO] Starting CPU intensive work #%u...\n", ++work_count);
        
        /* Simulate heavy CPU work that could cause priority inversion */
        for (volatile int i = 0; i < 3000000; i++) {
            /* Busy work - this could delay lower priority threads */
        }
        
        printk("[MEDIUM-PRIO] ✓ CPU work #%u completed\n", work_count);
        k_sleep(K_MSEC(800));
    }
}

/* Low priority thread */
static void low_priority_task(void *p1, void *p2, void *p3)
{
    char msg[50];
    int ret;
    
    printk("[LOW-PRIO] Thread started (Priority 15 - Preemptible)\n");
    
    /* Delay start to let other threads initialize */
    k_sleep(K_SECONDS(1));
    
    while (1) {
        low_prio_msg_count++;
        snprintf(msg, sizeof(msg), "low-prio msg #%u\r\n", low_prio_msg_count);
        
        printk("[LOW-PRIO] Sending message #%u...\n", low_prio_msg_count);
        
        /* Send synchronously to test priority inheritance */
        ret = uart_send_queued(msg, strlen(msg), 15, true);
        if (ret == 0) {
            printk("[LOW-PRIO] ✓ Message sent successfully\n");
        } else {
            printk("[LOW-PRIO] ✗ Message failed: %d\n", ret);
        }
        
        k_sleep(K_SECONDS(3));
    }
}

/* Statistics monitoring thread */
static void stats_thread(void *p1, void *p2, void *p3)
{
    while (1) {
        k_sleep(K_SECONDS(15));
        
        printk("=== PRIORITY INHERITANCE STATISTICS ===\n");
        printk("High priority messages sent: %u\n", high_prio_msg_count);
        printk("Low priority messages sent: %u\n", low_prio_msg_count);
        printk("Queue contentions: %u\n", queue_contentions);
        printk("Queue utilization: %u/%u\n", 
               k_msgq_num_used_get(&uart_tx_queue),
               k_msgq_num_free_get(&uart_tx_queue) + k_msgq_num_used_get(&uart_tx_queue));
        printk("========================================\n");
    }
}

/* Thread definitions with different priorities */
K_THREAD_DEFINE(uart_worker, 1024, uart_worker_thread, NULL, NULL, NULL,
                K_PRIO_COOP(3), 0, 0);    /* Highest priority - handles all UART */

K_THREAD_DEFINE(high_thread, 1024, high_priority_task, NULL, NULL, NULL,
                K_PRIO_COOP(5), 0, 0);    /* High priority sender */

K_THREAD_DEFINE(med_thread, 1024, medium_priority_task, NULL, NULL, NULL,
                K_PRIO_PREEMPT(10), 0, 0);  /* Medium priority - test priority inversion */

K_THREAD_DEFINE(low_thread, 1024, low_priority_task, NULL, NULL, NULL,
                K_PRIO_PREEMPT(15), 0, 0);  /* Low priority sender */

K_THREAD_DEFINE(stats_monitor, 512, stats_thread, NULL, NULL, NULL,
                K_PRIO_PREEMPT(20), 0, 0);  /* Statistics monitoring */

int main(void)
{
    int ret;
    
    printk("=== DMA UART with Dedicated Thread + Priority Inheritance Protection ===\n");
    
    /* Initialize synchronization primitives */
    k_mutex_init(&uart_queue_mutex);     /* Priority inheritance enabled by default */
    k_sem_init(&uart_tx_complete_sem, 0, 1);
    k_sem_init(&uart_rx_ready_sem, 0, 1);
    
    /* Initialize UART device */
    uart_dev = DEVICE_DT_GET(UART_NODE);
    if (!device_is_ready(uart_dev)) {
        printk("✗ UART device not ready\n");
        return -1;
    }
    printk("✓ UART device ready\n");
    
    /* Verify DMA configuration */
    verify_dma_usage();
    
    /* Set UART callback for DMA events */
    ret = uart_callback_set(uart_dev, uart_callback, NULL);
    if (ret != 0) {
        printk("✗ Failed to set UART callback: %d\n", ret);
        return ret;
    }
    printk("✓ UART callback registered\n");
    
    /* Start DMA RX */
    ret = uart_rx_enable(uart_dev, rx_buffer, sizeof(rx_buffer), SYS_FOREVER_US);
    if (ret != 0) {
        printk("✗ Failed to start DMA RX: %d\n", ret);
        return ret;
    }
    printk("✓ DMA RX started\n");
    
    printk("System initialized:\n");
    printk("- UART Worker Thread: Priority 3 (handles all DMA operations)\n");
    printk("- High Priority Thread: Priority 5 (sends messages every 2s)\n");
    printk("- Medium Priority Thread: Priority 10 (CPU intensive - tests priority inversion)\n");
    printk("- Low Priority Thread: Priority 15 (sends messages every 3s)\n");
    printk("- Message Queue: Protected by mutex with priority inheritance\n");
    printk("- Watch for priority inheritance in action!\n");
    
    /* Main thread monitors system */
    while (1) {
        k_sleep(K_SECONDS(30));
        printk("=== MAIN THREAD: System running normally ===\n");
    }
    
    return 0;
}