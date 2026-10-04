#include "gmb/scene.hpp"
#include <chrono>
#include <iostream>
#include <string>
int main() {
  for (const int count : {10000, 30000}) {
    for (const bool cycle : {false,true}) {
      gmb::Scene scene;
      scene.meshes.emplace_back();
      scene.materials.emplace_back();
      auto& mesh=scene.meshes.back();
      mesh.vertices.resize(3);
      mesh.vertices[1].position={1,0,0};
      mesh.vertices[2].position={0,1,0};
      mesh.triangles.push_back({{0,1,2},0});
      scene.nodes.resize(count);
      for(int i=0;i<count-1;++i) scene.nodes[i].parent=i+1;
      if(cycle) scene.nodes.back().parent=count/2;
      const auto start=std::chrono::steady_clock::now();
      const auto diagnostics=gmb::validate(scene);
      const auto end=std::chrono::steady_clock::now();
      std::cout << count << " " << (cycle?"tail-cycle":"valid-chain") << " "
                << std::chrono::duration<double,std::milli>(end-start).count()
                << " ms " << diagnostics.size() << " diagnostics\n";
    }
  }
}
