-- Create the web application's database, its two restricted MySQL users and
-- their grants. Run once against your running MySQL as an administrative user:
--
--   docker exec -i tgloggerd-mysql mysql -uroot -proot < web/docker/web-db-init.sql
--
-- Then apply the web migrations and seed an admin (see web/README.web,
-- "Running in Docker"). Change the passwords below to match the web service's
-- WEB_DB_RO_PASSWORD / WEB_DB_APP_PASSWORD environment variables.

CREATE DATABASE IF NOT EXISTS tgloggerd_web
  CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;

-- Read-only browsing over the logger's schema.
CREATE USER IF NOT EXISTS 'web_ro'@'%'  IDENTIFIED BY 'web_ro';
GRANT SELECT ON tgloggerd.* TO 'web_ro'@'%';

-- Read-write over the web app's own schema. ALL is needed to run the web
-- migrations; at run time the app performs only DML.
CREATE USER IF NOT EXISTS 'web_app'@'%' IDENTIFIED BY 'web_app';
GRANT ALL PRIVILEGES ON tgloggerd_web.* TO 'web_app'@'%';

FLUSH PRIVILEGES;
