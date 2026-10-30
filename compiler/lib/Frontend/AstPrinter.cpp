#include "nirqoba/Frontend/AstPrinter.h"

#include <sstream>

namespace nirq {
namespace {

class Printer {
 public:
  std::string print(const Program& program) {
    out_ << "program\n";
    for (const auto& function : program.functions()) {
      print_function(*function);
    }
    return out_.str();
  }

 private:
  void indent() {
    for (int level = 0; level < depth_; ++level) out_ << "  ";
  }

  void print_function(const FunctionDecl& function) {
    indent();
    out_ << "function @" << function.name() << " : " << function.return_type().to_string();
    if (!function.return_type().is_valid()) out_ << " (error)";
    out_ << " (" << function.location().line << ":" << function.location().column << ")\n";
    ++depth_;
    if (function.parameters().empty()) {
      indent();
      out_ << "params: none\n";
    }
    for (const Parameter& parameter : function.parameters()) {
      indent();
      out_ << "param " << parameter.name << " : " << parameter.type.to_string() << "\n";
    }
    print_block(function.body());
    --depth_;
  }

  void print_block(const BlockStmt& block) {
    indent();
    out_ << "block\n";
    ++depth_;
    for (const StmtPtr& statement : block.statements()) {
      print_statement(*statement);
    }
    --depth_;
  }

  void print_statement(const Stmt& statement) {
    switch (statement.kind()) {
      case Stmt::Kind::Block:
        print_block(static_cast<const BlockStmt&>(statement));
        return;
      case Stmt::Kind::VariableDeclaration: {
        const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
        indent();
        out_ << binding_kind_spelling(declaration.binding()) << " " << declaration.name();
        if (declaration.has_annotation()) {
          out_ << " : " << declaration.annotation().to_string();
        }
        out_ << " = " << expression(declaration.initializer()) << "\n";
        return;
      }
      case Stmt::Kind::Assignment: {
        const auto& assignment = static_cast<const AssignmentStmt&>(statement);
        indent();
        out_ << "assign " << assignment.name() << " = " << expression(assignment.value()) << "\n";
        return;
      }
      case Stmt::Kind::Return: {
        const auto& return_statement = static_cast<const ReturnStmt&>(statement);
        indent();
        out_ << "return";
        if (return_statement.has_value()) out_ << " " << expression(*return_statement.value());
        out_ << "\n";
        return;
      }
      case Stmt::Kind::Expression: {
        const auto& expression_statement = static_cast<const ExprStmt&>(statement);
        indent();
        out_ << "expr " << expression(expression_statement.expr()) << "\n";
        return;
      }
    }
  }

  std::string expression(const Expr& expr) {
    switch (expr.kind()) {
      case Expr::Kind::IntegerLiteral:
        return std::to_string(static_cast<const IntegerLiteralExpr&>(expr).value());
      case Expr::Kind::StringLiteral: {
        const std::string& value = static_cast<const StringLiteralExpr&>(expr).value();
        std::string escaped;
        for (const char c : value) {
          switch (c) {
            case '\n': escaped += "\n"; break;
            case '\t': escaped += "\t"; break;
            case '\r': escaped += "\r"; break;
            case '"': escaped += "\\\""; break;
            case '\': escaped += "\\\\"; break;
            default: escaped.push_back(c); break;
          }
        }
        return "\"" + escaped + "\"";
      }
      case Expr::Kind::BoolLiteral:
        return static_cast<const BoolLiteralExpr&>(expr).value() ? "true" : "false";
      case Expr::Kind::Name:
        return static_cast<const NameExpr&>(expr).name();
      case Expr::Kind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        std::string text = call.callee() + "(";
        for (std::size_t index = 0; index < call.arguments().size(); ++index) {
          if (index != 0) text += ", ";
          text += expression(*call.arguments()[index]);
        }
        return text + ")";
      }
      case Expr::Kind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expr);
        return "(" + std::string(unary_op_spelling(unary.op())) + expression(unary.operand()) + ")";
      }
      case Expr::Kind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expr);
        // Fully parenthesized so precedence bugs cannot hide in the dump.
        return "(" + expression(binary.lhs()) + " " + std::string(binary_op_spelling(binary.op())) +
               " " + expression(binary.rhs()) + ")";
      }
    }
    return "<unknown>";
  }

  std::ostringstream out_;
  int depth_ = 0;
};

}  // namespace

std::string print_program(const Program& program) {
  Printer printer;
  return printer.print(program);
}

}  // namespace nirq
