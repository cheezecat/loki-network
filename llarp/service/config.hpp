#ifndef LLARP_SERVICE_CONFIG_HPP
#define LLARP_SERVICE_CONFIG_HPP
#include <list>
#include <string>

namespace llarp
{
  namespace service
  {
    struct Config
    {
      using section_values_t =
          std::list< std::pair< std::string, std::string > >;
      using section_t = std::pair< std::string, section_values_t >;

      std::list< section_t > services;

      bool
      Load(const std::string& fname);
    };
  }  // namespace service
}  // namespace llarp
#endif
