DROP DATABASE IF EXISTS wzg_probe_crud_tx_test;
CREATE DATABASE wzg_probe_crud_tx_test;
USE wzg_probe_crud_tx_test;

CREATE TABLE users (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(64) NOT NULL,
  status VARCHAR(16) NOT NULL,
  balance DECIMAL(10,2) NOT NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  KEY idx_status (status),
  KEY idx_balance (balance)
) ENGINE=InnoDB;

CREATE TABLE orders (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  user_id BIGINT NOT NULL,
  amount DECIMAL(10,2) NOT NULL,
  state VARCHAR(16) NOT NULL,
  KEY idx_user_state (user_id, state),
  KEY idx_amount (amount),
  CONSTRAINT fk_orders_user FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB;

INSERT INTO users(name, status, balance) VALUES
  ('Alice', 'active', 100.00),
  ('Bob', 'active', 40.00),
  ('Cathy', 'blocked', 5.00);

INSERT INTO orders(user_id, amount, state) VALUES
  (1, 25.50, 'paid'),
  (1, 70.00, 'new'),
  (2, 18.00, 'new');

SELECT u.id, u.name, o.amount
FROM users u JOIN orders o ON o.user_id = u.id
WHERE u.status = 'active' AND o.amount >= 20
ORDER BY o.amount DESC;

START TRANSACTION;
SELECT id, balance FROM users WHERE id = 1 FOR UPDATE;
UPDATE users SET balance = balance - 10 WHERE id = 1;
INSERT INTO orders(user_id, amount, state) VALUES (1, 10.00, 'new');
COMMIT;

START TRANSACTION;
UPDATE users SET status = 'blocked' WHERE id = 2;
DELETE FROM orders WHERE user_id = 2 AND state = 'new';
ROLLBACK;

SELECT id, name, status, balance
FROM users
WHERE id IN (1, 2, 3)
ORDER BY id;
