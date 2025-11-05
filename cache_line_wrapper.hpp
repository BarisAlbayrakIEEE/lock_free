
        // A wrapper
        struct alignas(std::hardware_destructive_interference_size) CL {
            std::atomic<std::uint64_t> v{0};
        };
