import Foundation

enum GNFSInvocation {
  static func arguments(for configuration: RunConfiguration) -> [String] {
    var arguments = [
      configuration.number,
      "--event-stream",
      "--complete",
      "--lang", "zh",
      "--method", configuration.method.rawValue,
    ]

    let parameters = configuration.parameters
    append(parameters.degree, flag: "--degree", to: &arguments)
    append(parameters.rationalBound, flag: "--fb-rational", to: &arguments)
    append(parameters.algebraicBound, flag: "--fb-algebraic", to: &arguments)
    append(parameters.largePrimeBound, flag: "--lp-bound", to: &arguments)
    append(parameters.sieveWidth, flag: "--sieve-width", to: &arguments)
    append(parameters.sieveHeight, flag: "--sieve-height", to: &arguments)
    return arguments
  }

  private static func append<T: LosslessStringConvertible>(
    _ value: T?,
    flag: String,
    to arguments: inout [String]
  ) {
    guard let value else { return }
    arguments.append(contentsOf: [flag, String(value)])
  }
}
