#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/sys/printk.h>

/* Device tree nodes */
#define UART_NODE DT_CHOSEN(zephyr_console)

/* Mutex for resource protection (with priority inheritance) */
static struct k_mutex uart_resource_mutex;

/* Semaphores for event signaling */
static struct k_sem uart_tx_sem;
static struct k_sem uart_rx_sem;

/* UART device */
static const struct device *uart_dev;

/* Shared resources protected by mutex */
static char tx_buffer[64];
static uint8_t rx_buffer[64];
static uint8_t rx_double_buffer[64];
static volatile bool uart_tx_busy = false;
static volatile bool use_rx_buffer_1 = true;

/* Verify DMA is being used */
static void verify_dma_usage(void)
{
    /* Check if UART driver has DMA configured */
    printk("Checking DMA configuration...\n");
    
    /* This will show in logs if DMA channels are allocated */
    /* Look for messages like "DMA channel X allocated" */
    
    /* You can also check device tree at runtime */
    #if DT_NODE_HAS_PROP(UART_NODE, dmas)
        printk("✓ UART has DMA configured in device tree\n");
    #else
        printk("⚠ UART does NOT have DMA in device tree\n");
    #endif
}

/* UART callback - confirms DMA completion events */
static void uart_callback(const struct device *dev, struct uart_event *evt, void *user_data)
{
    switch (evt->type) {
    case UART_TX_DONE:
        printk("✓ DMA TX completed - %d bytes\n", evt->data.tx.len);
        k_sem_give(&uart_tx_sem);
        break;
        
    case UART_TX_ABORTED:
        printk("✗ DMA TX aborted\n");
        k_sem_give(&uart_tx_sem);
        break;
        
    case UART_RX_RDY:
        printk("✓ DMA RX ready - %d bytes at offset %d\n", 
               evt->data.rx.len, evt->data.rx.offset);
        
        /* Process received data */
        uint8_t *data = &evt->data.rx.buf[evt->data.rx.offset];
        printk("Received via DMA: ");
        for (int i = 0; i < evt->data.rx.len; i++) {
            printk("%c", data[i]);
        }
        printk("\n");
        
        k_sem_give(&uart_rx_sem);
        break;
        
    case UART_RX_BUF_REQUEST:
        printk("✓ DMA RX buffer request (continuous DMA)\n");
        /* Provide next buffer for continuous DMA operation */
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

/* Send data with DMA and priority protection */
static int uart_send_dma_protected(const char *data, size_t len)
{
    int ret;
    
    if (len > sizeof(tx_buffer)) {
        printk("Message too long for buffer\n");
        return -EINVAL;
    }
    
    /* Step 1: Acquire mutex for resource protection (priority inheritance) */
    ret = k_mutex_lock(&uart_resource_mutex, K_MSEC(1000));
    if (ret != 0) {
        printk("Failed to acquire UART mutex: %d\n", ret);
        return ret;
    }
    
    /* Step 2: Check if UART TX is busy */
    if (uart_tx_busy) {
        k_mutex_unlock(&uart_resource_mutex);
        return -EBUSY;
    }
    
    /* Step 3: Mark as busy and copy to DMA-safe buffer */
    uart_tx_busy = true;
    memcpy(tx_buffer, data, len);
    
    /* Step 4: Start DMA TX operation */
    printk("Starting DMA TX operation (%d bytes)...\n", len);
    ret = uart_tx(uart_dev, (uint8_t *)tx_buffer, len, SYS_FOREVER_US);
    if (ret != 0) {
        printk("DMA TX start failed: %d\n", ret);
        uart_tx_busy = false;
        k_mutex_unlock(&uart_resource_mutex);
        return ret;
    }
    
    /* Step 5: Release mutex - DMA operation is now running independently */
    k_mutex_unlock(&uart_resource_mutex);
    
    /* Step 6: Wait for DMA completion via semaphore */
    ret = k_sem_take(&uart_tx_sem, K_MSEC(5000));
    if (ret != 0) {
        printk("DMA TX timeout - operation may still be running\n");
        return ret;
    }
    
    /* Step 7: Clear busy flag */
    k_mutex_lock(&uart_resource_mutex, K_FOREVER);
    uart_tx_busy = false;
    k_mutex_unlock(&uart_resource_mutex);
    
    printk("DMA TX operation completed successfully\n");
    return 0;
}

/* Start DMA RX with protection */
static int uart_start_dma_rx(void)
{
    int ret;
    
    k_mutex_lock(&uart_resource_mutex, K_FOREVER);
    
    printk("Starting continuous DMA RX...\n");
    ret = uart_rx_enable(uart_dev, rx_buffer, sizeof(rx_buffer), SYS_FOREVER_US);
    if (ret != 0) {
        printk("Failed to start DMA RX: %d\n", ret);
    } else {
        printk("✓ DMA RX started successfully\n");
    }
    
    k_mutex_unlock(&uart_resource_mutex);
    return ret;
}

/* High priority thread */
static void high_priority_task(void *p1, void *p2, void *p3)
{
    int msg_count = 0;
    
    while (1) {
        char msg[50];
        snprintf(msg, sizeof(msg), "HIGH-PRI MSG #%d\r\n", ++msg_count);
        
        printk("[HIGH] Sending message via DMA...\n");
        if (uart_send_dma_protected(msg, strlen(msg)) == 0) {
            printk("[HIGH] ✓ DMA message sent successfully\n");
        } else {
            printk("[HIGH] ✗ DMA message failed\n");
        }
        
        k_sleep(K_SECONDS(3));
    }
}

/* Medium priority CPU intensive thread */
static void medium_priority_task(void *p1, void *p2, void *p3)
{
    while (1) {
        printk("[MED] Starting CPU intensive work...\n");
        
        /* Simulate CPU intensive work */
        for (volatile int i = 0; i < 2000000; i++) {
            /* This could cause priority inversion without proper protection */
        }
        
        printk("[MED] CPU work completed\n");
        k_sleep(K_MSEC(500));
    }
}

/* Low priority thread */
static void low_priority_task(void *p1, void *p2, void *p3)
{
    int msg_count = 0;
    
    k_sleep(K_SECONDS(1)); /* Start after others */
    
    while (1) {
        char msg[50];
        snprintf(msg, sizeof(msg), "low-pri msg #%d\r\n", ++msg_count);
        
        printk("[LOW] Sending message via DMA...\n");
        if (uart_send_dma_protected(msg, strlen(msg)) == 0) {
            printk("[LOW] ✓ DMA message sent successfully\n");
        } else {
            printk("[LOW] ✗ DMA message failed\n");
        }
        
        k_sleep(K_SECONDS(4));
    }
}

/* Thread definitions with different priorities */
K_THREAD_DEFINE(high_thread, 1024, high_priority_task, NULL, NULL, NULL,
                K_PRIO_COOP(5), 0, 0);   /* Cooperative, highest priority */

K_THREAD_DEFINE(med_thread, 1024, medium_priority_task, NULL, NULL, NULL,
                K_PRIO_PREEMPT(10), 0, 0); /* Preemptible, medium priority */

K_THREAD_DEFINE(low_thread, 1024, low_priority_task, NULL, NULL, NULL,
                K_PRIO_PREEMPT(15), 0, 0); /* Preemptible, lowest priority */

int main(void)
{
    int ret;
    
    printk("=== DMA UART with Priority Inversion Protection ===\n");
    
    /* Initialize synchronization primitives */
    k_mutex_init(&uart_resource_mutex);  /* Priority inheritance enabled */
    k_sem_init(&uart_tx_sem, 0, 1);
    k_sem_init(&uart_rx_sem, 0, 1);
    
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
    if (uart_start_dma_rx() != 0) {
        printk("✗ Failed to start DMA RX\n");
        return -1;
    }
    
    printk("System initialized - DMA TX/RX active with priority protection\n");
    printk("You can type messages to test DMA RX\n");
    printk("Watching for priority inversion scenarios...\n");
    
    /* Main thread monitors system */
    while (1) {
        k_sleep(K_SECONDS(10));
        printk("=== System Status: DMA operations running ===\n");
    }
    
    return 0;
}