/**
 * dpdk应用底座
 *
 * @date 2026/04/21
 * @author ZhangAo
 */

#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <libconfig.h>

#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>

#define PROMISCUOUS_ON 1 // 是否启用混杂模式
#define MBUF_BUF_SIZE 9216
#define MEMPOOL_CACHE_SIZE 256
#define MAX_PKT_BURST 32
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024
static uint16_t nb_rxd = RX_DESC_DEFAULT;
static uint16_t nb_txd = TX_DESC_DEFAULT;
struct rte_mempool *app_pktmbuf_pool = NULL;
static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];
static struct rte_eth_conf port_conf = {
    .txmode = {
        .mq_mode = RTE_ETH_MQ_TX_NONE,
    },
    .rxmode = {
        .max_lro_pkt_size = MBUF_BUF_SIZE,
    }};
static struct rte_ether_addr app_ports_eth_addr[RTE_MAX_ETHPORTS];
struct __rte_cache_aligned app_port_statistics
{
    uint64_t tx;
    uint64_t rx;
    uint64_t dropped;
};
struct app_port_statistics port_statistics[RTE_MAX_ETHPORTS];

#define APP_CONFIG_FILE "app_config.cfg"
struct app_config
{
    char *name;
    char *version;
};
struct app_config app_config;
static volatile bool force_quit;
uint64_t TSC_1S = 0;

/* App Init */
static void dpdk_init(int argc, char **argv)
{
    int ret;
    uint16_t portid;
    uint16_t nb_ports;
    unsigned int nb_lcores;
    unsigned int nb_mbufs;

    // Init EAL
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");

    // Init dev
    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No Ethernet ports\n");

    nb_lcores = 2;
    nb_mbufs =
        RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST + nb_lcores * MEMPOOL_CACHE_SIZE),
                8192U);

    app_pktmbuf_pool = rte_pktmbuf_pool_create(
        "mbuf_pool", nb_mbufs,
        MEMPOOL_CACHE_SIZE, 0, MBUF_BUF_SIZE,
        rte_socket_id());
    if (app_pktmbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

    RTE_ETH_FOREACH_DEV(portid)
    {
        struct rte_eth_rxconf rxq_conf;
        struct rte_eth_txconf txq_conf;
        struct rte_eth_conf local_port_conf = port_conf;
        struct rte_eth_dev_info dev_info;

        ret = rte_eth_dev_info_get(portid, &dev_info);
        if (ret != 0)
            rte_exit(EXIT_FAILURE,
                     "Error during getting device (port %u) info: %s\n",
                     portid, strerror(-ret));

        if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
            local_port_conf.txmode.offloads |=
                RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

        ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
        if (ret < 0)
            rte_exit(EXIT_FAILURE,
                     "Cannot configure device: err=%d, port=%u\n",
                     ret, portid);

        ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
        if (ret < 0)
            rte_exit(EXIT_FAILURE,
                     "Cannot adjust number of descriptors: err=%d, port=%u\n",
                     ret, portid);

        ret = rte_eth_macaddr_get(portid, &app_ports_eth_addr[portid]);
        if (ret < 0)
            rte_exit(EXIT_FAILURE,
                     "Cannot get MAC address: err=%d, port=%u\n",
                     ret, portid);

        rxq_conf = dev_info.default_rxconf;
        rxq_conf.offloads = local_port_conf.rxmode.offloads;

        ret = rte_eth_rx_queue_setup(
            portid, 0, nb_rxd,
            rte_eth_dev_socket_id(portid),
            &rxq_conf,
            app_pktmbuf_pool);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
                     ret, portid);

        txq_conf = dev_info.default_txconf;
        txq_conf.offloads = local_port_conf.txmode.offloads;

        ret = rte_eth_tx_queue_setup(
            portid, 0, nb_txd,
            rte_eth_dev_socket_id(portid),
            &txq_conf);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
                     ret, portid);

        tx_buffer[portid] = rte_zmalloc_socket(
            "tx_buffer",
            RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
            rte_eth_dev_socket_id(portid));
        if (tx_buffer[portid] == NULL)
            rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
                     portid);

        rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

        ret = rte_eth_tx_buffer_set_err_callback(
            tx_buffer[portid],
            rte_eth_tx_buffer_count_callback,
            &port_statistics[portid].dropped);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot set error callback for tx buffer on port %u\n",
                     portid);

        ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL, 0);
        if (ret < 0)
            printf("Port %u, Failed to disable Ptype parsing\n", portid);

        ret = rte_eth_dev_start(portid);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
                     ret, portid);

        if (PROMISCUOUS_ON)
        {
            ret = rte_eth_promiscuous_enable(portid);
            if (ret != 0)
                rte_exit(EXIT_FAILURE,
                         "rte_eth_promiscuous_enable:err=%s, port=%u\n",
                         rte_strerror(-ret), portid);
        }

        printf("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT "\n\n",
               portid,
               RTE_ETHER_ADDR_BYTES(&app_ports_eth_addr[portid]));

        memset(&port_statistics, 0, sizeof(port_statistics));
    }

    fflush(stdout);
}

static void load_config()
{
    printf("[INFO] 加载配置...\n");

    config_t cfg;
    config_init(&cfg);

    if (!config_read_file(&cfg, APP_CONFIG_FILE))
    {
        fprintf(stderr, "%s:%d: %s\n", config_error_file(&cfg),
                config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
    }

    const char *version;
    const char *name;
    config_lookup_string(&cfg, "app_config.version", &version);
    config_lookup_string(&cfg, "app_config.name", &name);

    app_config.name = strdup(name);
    app_config.version = strdup(version);

    config_destroy(&cfg);
}

static void var_init()
{
    printf("[INFO] 初始化变量...\n");
    TSC_1S = rte_get_tsc_hz();
}

/* App */
static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        printf("\nPreparing to exit...\n");
        force_quit = true;
    }
}

int main(int argc, char **argv)
{
    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Init
    dpdk_init(argc, argv);
    load_config();
    var_init();
    printf("\n=== %s @%s ===\n",
           app_config.name, app_config.version);

    // Start

    return 0;
}